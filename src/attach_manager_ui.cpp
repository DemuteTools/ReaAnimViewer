// SPDX-License-Identifier: MIT
//
// See attach_manager_ui.h (13-68e).

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
#include "shortcuts.h"      // LoadPrefFloat / SavePrefFloat
#include "sound_project.h"  // GetSoundStore
#include "sound_record.h"
#include "sound_spread_plan.h"
#include "sound_spread_selection.h"
#include "ui_flow.h"
#include "ui_theme.h"

namespace rav {

namespace {

const char kDot[] = " \xC2\xB7 ";
constexpr const char kPrefScope[] = "attachmgr.scope_all";
constexpr const char kPrefSide[] = "attachmgr.side_only";
constexpr const char kPrefOther[] = "attachmgr.other_none";    // 13-68e v4: the other side plays nothing
constexpr const char kPrefDetachSide[] = "attachmgr.detach_side";  // 13-68e v4: 0 Both, 1 L, 2 R

// A little wider than the mockup's 370 px window; its size is kept, it opens at the mouse.
constexpr int kDefaultW = 380, kDefaultH = 540;
constexpr int kMinW = 300, kMinH = 260;
FloatUiWindow g_host("attachmgr.float", L"Attach manager", kDefaultW, kDefaultH, kMinW, kMinH);

double ClockSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- Icons (the mockup's 16 px SVGs) --------------------------------------------------------------------------

enum AmGlyph : int { kGlyphLink = 0, kGlyphUnlink, kGlyphTarget };

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

void DrawAmGlyph(ImDrawList* dl, ImVec2 p, float s, int which, ImU32 col)
{
    const float u = s / 16.0f;
    auto        at = [&](float x, float y) { return ImVec2(p.x + x * u, p.y + y * u); };
    switch (which) {
    case kGlyphLink:
    case kGlyphUnlink:
        Capsule(dl, at(5.6f, 10.4f), u, col);
        Capsule(dl, at(10.4f, 5.6f), u, col);
        if (which == kGlyphUnlink) dl->AddLine(at(2.5f, 2.5f), at(13.5f, 13.5f), col, std::max(1.0f, 1.5f * u));
        break;
    case kGlyphTarget:
        dl->AddCircle(at(8.0f, 8.0f), 5.5f * u, col, 20, std::max(1.0f, 1.4f * u));
        dl->AddCircleFilled(at(8.0f, 8.0f), 1.8f * u, col, 12);
        break;
    default: break;
    }
}

ui::Icon AmIcon(int which, ImU32 col = ui::kMuted)
{
    return ui::Icon{DrawAmGlyph, which, col};
}

// ---- State ----------------------------------------------------------------------------------------------------

struct Ask {
    int                       type = 0;  // 1 a marker, 2 the markers shown, 3 a variation
    std::vector<DetachTarget> targets;
    std::string               lib, node;
    char                      side = 0;  // 13-68e v4: Remove from library on that side only
    std::string               title, sub;
    ImVec2                    at;
};

struct State {
    bool               open = false;
    bool               focus = false;
    bool               place = false;
    int                tab = 0;  // 0 Attach, 1 Detach
    AttachManagerState st;
    bool               dirty = true;
    std::string        sig, clicked;
    double             next_poll = 0.0;
    // Attach
    std::string        pick_key;  // the user's Marker pick
    am::GestureWant    want = am::GestureWant::Auto;
    bool               scope_all = false, side_only = false;
    bool               other_none = false;  // 13-68e v4: with "<side> only", the other side plays nothing
    int                dside = 0;           // 13-68e v4: the Detach tab's Side: 0 Both, 1 L, 2 R
    bool               prefs_read = false;
    // Detach
    std::string        lib_pick;  // "" = All libraries
    int                mode = 0;  // 0 This marker, 1 Whole library
    // A selection made from the list keeps its filter until the user selects something else in REAPER.
    bool               frozen = false;
    bool               frozen_take = false;  // the next read takes the selection it made
    am::ListFilter     frozen_filter;
    std::set<std::string> frozen_sel;
    std::string        sel_row;  // the row last selected from the list (its key)
    Ask                ask;
    bool               ask_open = false;
    bool               ask_close = false;  // close the question from inside its popup (DrawAsk)
    bool               force = false;      // read now (a gesture ran, the window opened): no throttle
    double             next_read = 0.0;    // a project change is read at most every 0.25 s
    // A gesture clicked, run from the viewer's window procedure.
    std::function<void()> pending;
};
State g;

void ReadPrefs()
{
    if (g.prefs_read) return;
    g.prefs_read = true;
    g.scope_all = LoadPrefFloat(kPrefScope, 0.0f) > 0.5f;
    g.side_only = LoadPrefFloat(kPrefSide, 0.0f) > 0.5f;
    g.other_none = LoadPrefFloat(kPrefOther, 0.0f) > 0.5f;
    g.dside = std::max(0, std::min(2, static_cast<int>(std::lround(LoadPrefFloat(kPrefDetachSide, 0.0f)))));
}

// The snapshot again when the project, the time selection, the item selection or a clicked label moved.
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
    if (g.frozen_take) {
        g.frozen_take = false;
        g.frozen_sel = g.st.snap.selected_items;
    } else if (g.frozen && !g.pending && g.st.snap.selected_items != g.frozen_sel) {
        g.frozen = false;  // a selection made in the arrange: the list follows it again
        g.sel_row.clear();
    }
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

void CloseAsk()
{
    if (g.ask_open) g.ask_close = true;  // DrawAsk closes the popup from inside it
}

std::string LibName(const std::string& uid)
{
    const SoundLibrary* l = FindLibrary(GetSoundStore(nullptr), uid);
    return l ? l->name : std::string();
}

// ---- Drawing helpers ------------------------------------------------------------------------------------------

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

// The mockup's side badge (L / R / + / a count): a small rounded box, its ink by kind.
void Badge(ImDrawList* dl, ImVec2 a, ImVec2 b, const std::string& text)
{
    ImU32 ink = ui::kMuted, edge = ui::kStrokeStrong;
    if (text == "L") {
        ink = IM_COL32(0xC7, 0xA8, 0xFF, 0xFF);
        edge = IM_COL32(0xA0, 0x6E, 0xE6, 0x80);
    } else if (text == "R") {
        ink = IM_COL32(0xF0, 0xA2, 0xC4, 0xFF);
        edge = IM_COL32(0xC8, 0x5A, 0x8C, 0x80);
    }
    dl->AddRect(a, b, edge, ui::Dp(4.0f));
    ImFont*      f = ui::FontBadge();
    const ImVec2 ts = ui::TextSizeIn(f, text.c_str());
    ui::AddTextIn(dl, f, ImVec2(std::floor((a.x + b.x - ts.x) * 0.5f), std::floor((a.y + b.y - ts.y) * 0.5f)), ink, text.c_str());
}

// One list row: badge, two lines, the select and detach buttons in a fixed column at its right (always shown: they
// never move). Returns 1 when the row or its select button was clicked, 2 for the detach button.
int ListRow(const char* id, const std::string& badge, const std::string& l1, const std::string& l2, bool selected,
            const char* sel_tip, const char* det_tip, ImVec2* det_at)
{
    ImGui::PushID(id);
    const float  btn = ui::ControlH();
    const float  gap = ui::Dp(2.0f);
    const float  col_w = 2.0f * btn + gap + ui::Dp(4.0f);
    const float  h = std::max(ui::RowH(), ImGui::GetTextLineHeight() + ui::TextSizeIn(ui::FontSmall(), "Ag").y + ui::Dp(8.0f));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = std::max(ui::Dp(80.0f), ImGui::GetContentRegionAvail().x);
    int          out = 0;
    if (ui::SelectRow("##row", selected, 0, h, ImGuiSelectableFlags_AllowOverlap) && !g.pending) out = 1;
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  bw = ui::Dp(22.0f);
    Badge(dl, ImVec2(p.x + ui::Dp(4.0f), p.y + (h - ui::Dp(16.0f)) * 0.5f), ImVec2(p.x + ui::Dp(4.0f) + bw, p.y + (h + ui::Dp(16.0f)) * 0.5f), badge);
    const float tx = p.x + ui::Dp(4.0f) + bw + ui::Dp(6.0f);
    const float tmax = p.x + w - col_w - ui::Dp(4.0f);
    const float lh1 = ImGui::GetTextLineHeight();
    const float lh2 = ui::TextSizeIn(ui::FontSmall(), "Ag").y;
    const float ty = p.y + (h - lh1 - lh2) * 0.5f;
    ui::EllipsisTextTip(dl, ImVec2(tx, ty), tmax, ui::kText, l1);
    ImGui::PushFont(ui::FontSmall());
    ui::EllipsisTextTip(dl, ImVec2(tx, ty + lh1), tmax, ui::kMuted, l2);
    ImGui::PopFont();
    // The fixed column.
    const float bx = p.x + w - 2.0f * btn - gap - ui::Dp(2.0f);
    ImGui::SetCursorScreenPos(ImVec2(bx, p.y + (h - btn) * 0.5f));
    const bool busy = static_cast<bool>(g.pending);  // a gesture waits: no other one
    if (busy) ImGui::BeginDisabled();
    if (ui::IconOnlyButton("##sel", AmIcon(kGlyphTarget), sel_tip)) out = 1;
    ImGui::SameLine(0.0f, gap);
    if (det_tip) {
        if (ui::IconOnlyButton("##det", AmIcon(kGlyphUnlink), det_tip)) {
            out = 2;
            if (det_at) *det_at = ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
        }
    }
    if (busy) ImGui::EndDisabled();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + ui::Dp(2.0f)));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopID();
    return out;
}

// The items of these events (what a marker row selects).
// 13-68e v4: the picked library (on All libraries: any Attached library) has sided markers.
bool DetachSided()
{
    const SoundStore& st = GetSoundStore(nullptr);
    for (const SoundLibrary& l : st.libraries)
        if (l.attached && (g.lib_pick.empty() || l.uid == g.lib_pick) && am::LibrarySided(st, l, g.st.snap)) return true;
    return false;
}

// 13-68e v4: the Detach tab's side ('L' / 'R'; 0 = both, also when the library's markers have no side).
char DetachSide()
{
    if (g.dside == 0 || !DetachSided()) return 0;
    return g.dside == 1 ? 'L' : 'R';
}

std::vector<std::string> EventItems(const std::vector<int>& events)
{
    std::vector<std::string> out;
    for (int i : events)
        if (i >= 0 && static_cast<size_t>(i) < g.st.snap.events.size())
            for (const am::Placed& p : g.st.snap.events[static_cast<size_t>(i)].placed) out.push_back(p.item_guid);
    return out;
}

// Selects from the list: the list keeps what it shows.
void SelectFromList(const std::vector<std::string>& guids, const am::ListFilter& shown)
{
    const bool queued = Queue([guids] {
        SelectItemsByGuid(guids);
        g.frozen_take = true;  // the next read keeps this selection as the list's own
    });
    if (!queued) return;
    if (!g.frozen) g.frozen_filter = shown;
    g.frozen = true;
}

DetachTarget TargetOf(const am::AttachedEvent& e)
{
    DetachTarget t;
    if (e.anim >= 0 && static_cast<size_t>(e.anim) < g.st.snap.anims.size()) t.owner = g.st.snap.anims[static_cast<size_t>(e.anim)].guid;
    t.rid = e.rid;
    t.t = e.t;
    char b[32];
    std::snprintf(b, sizeof(b), " %.2f s", e.pos);
    t.label = (e.label.empty() ? std::string("marker") : e.label) + b;
    return t;
}

void OpenAsk(Ask a)
{
    g.ask = std::move(a);
    g.ask_open = true;  // opened at the body's level (DrawContent), where DrawAsk begins it
}

// ---- The title row and the tabs ---------------------------------------------------------------------------------

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
    DrawAmGlyph(dl, ImVec2(p.x + ui::Dp(2.0f), std::floor(p.y + (h - ip) * 0.5f)), ip, kGlyphLink, ui::kMuted);
    const float ty = std::floor(p.y + (h - ImGui::GetTextLineHeight()) * 0.5f);
    float       x = p.x + ui::Dp(2.0f) + ip + ui::Dp(8.0f);
    ui::AddTextIn(dl, ui::FontSemibold(), ImVec2(x, ty), ui::kText, gesture.c_str());
    x += ui::TextSizeIn(ui::FontSemibold(), gesture.c_str()).x;
    if (!what.empty()) ui::EllipsisTextTip(dl, ImVec2(x, ty), p.x + w - close_w - ui::Dp(8.0f), ui::kMuted, std::string(kDot) + what);
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - close_w, p.y + (h - close_w) * 0.5f));
    if (ui::IconOnlyButton("##amclose", ToolIcon(kToolCross), "Close (Esc)")) g.open = false;
    dl->AddLine(ImVec2(p.x - ui::Dp(8.0f), p.y + h), ImVec2(p.x + w + ui::Dp(8.0f), p.y + h), ui::kStroke);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + ui::Dp(2.0f)));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

void DrawTabs()
{
    static const char* const kTabs[2] = {"Attach", "Detach"};
    static const char* const kTips[2] = {"RAV: Attach manager - Attach (at mouse)", "RAV: Attach manager - Detach (at mouse)"};
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float  h = ui::Dp(28.0f);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    float        x = p0.x;
    for (int i = 0; i < 2; ++i) {
        const float tw = ImGui::CalcTextSize(kTabs[i]).x + ui::Dp(24.0f);
        ImGui::SetCursorScreenPos(ImVec2(x, p0.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(tw, h)) && g.tab != i) {
            g.tab = i;
            CloseAsk();
        }
        const bool hov = ImGui::IsItemHovered();
        ui::ItemTipText(kTips[i]);
        ImGui::PopID();
        const bool on = g.tab == i;
        dl->AddText(ImVec2(x + ui::Dp(12.0f), std::floor(p0.y + (h - ImGui::GetTextLineHeight()) * 0.5f)),
                    on || hov ? ui::kText : ui::kMuted, kTabs[i]);
        if (on) dl->AddRectFilled(ImVec2(x, p0.y + h - ui::Dp(2.0f)), ImVec2(x + tw, p0.y + h), ui::kAccent);
        x += tw + ui::Dp(2.0f);
    }
    dl->AddLine(ImVec2(p0.x - ui::Dp(8.0f), p0.y + h), ImVec2(p0.x + w + ui::Dp(8.0f), p0.y + h), ui::kStroke);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h + ui::Dp(8.0f)));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

// ---- Attach tab ---------------------------------------------------------------------------------------------------

struct AttachView {
    std::vector<int>  choices;
    int               marker = -1;
    am::GestureChoice gc;
    std::string       kind;   // the kind's side-free name
    char              side = 0;
    std::string       target; // "FS L · 1.70 s"
    std::string       anim;   // the marker's animation
};

AttachView MakeAttachView()
{
    AttachView        v;
    const am::Snapshot& s = g.st.snap;
    v.choices = am::MarkerChoices(s);
    v.marker = am::PreselectMarker(s, v.choices, g.pick_key);
    v.gc = am::ChooseGesture(s, v.choices, v.marker, g.want);
    if (v.gc.gesture == am::Gesture::Stack) {
        if (v.gc.stack_event >= 0 && static_cast<size_t>(v.gc.stack_event) < s.events.size()) {
            const am::AttachedEvent& e = s.events[static_cast<size_t>(v.gc.stack_event)];
            v.kind = LibName(e.lib_uid);
            v.side = e.side;
            char b[32];
            std::snprintf(b, sizeof(b), "%.2f s", e.pos);
            v.target = e.label + kDot + b;
            if (e.anim >= 0) v.anim = s.anims[static_cast<size_t>(e.anim)].name;
        }
    } else if (v.marker >= 0) {
        const am::Marker& m = s.markers[static_cast<size_t>(v.marker)];
        v.kind = m.kind_name;
        v.side = m.side;
        v.target = am::MarkerText(m);
        if (m.anim >= 0) v.anim = s.anims[static_cast<size_t>(m.anim)].name;
    }
    if (v.kind.empty()) v.kind = "marker";
    return v;
}

void RunAttach(const AttachView& v)
{
    const SpreadScope scope = g.scope_all ? SpreadScope::AllInstances : SpreadScope::ThisAnimation;
    const SpreadSide  side = g.side_only && v.side ? SpreadSide::MarkerSide : SpreadSide::Both;
    const bool        none = side == SpreadSide::MarkerSide && g.other_none;  // 13-68e v4
    if (v.gc.gesture == am::Gesture::Stack) {
        Queue([scope, side, none] { AddSelectionToStackSide(scope, side, none); });
    } else {
        const am::Marker& m = g.st.snap.markers[static_cast<size_t>(v.marker)];
        SpreadMarker      sm;
        sm.item = m.anim >= 0 && static_cast<size_t>(m.anim) < g.st.anims.size() ? g.st.anims[static_cast<size_t>(m.anim)] : nullptr;
        sm.name = m.name;
        sm.pos = m.pos;
        Queue([scope, side, sm, none] { SpreadSelectionToMarker(scope, side, &sm, none); });
    }
    g.want = am::GestureWant::Auto;
}

void DrawAttach(const AttachView& v)
{
    const am::Snapshot& s = g.st.snap;
    const float         col = std::max(ImGui::CalcTextSize("Marker").x, ImGui::CalcTextSize("Side").x);
    // Marker
    {
        const float fw = FieldLabel("Marker", col);
        const bool  stack = v.gc.gesture == am::Gesture::Stack;
        const bool  one = stack || v.choices.size() < 2;
        std::string preview = stack ? (v.target.empty() ? std::string("\xE2\x80\x94") : v.target)
                                    : v.marker >= 0 ? am::MarkerText(s.markers[static_cast<size_t>(v.marker)]) : std::string("\xE2\x80\x94");
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
        ui::FitText(v.anim.empty() ? std::string("\xE2\x80\x94") : v.anim, v.anim.empty() ? ui::kMuted : ui::kText);
        if (!v.gc.from.empty()) FaintLine(v.gc.from);
    }
    ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
    // Selection
    ui::Section       sec;
    std::string       count = s.selected_items.empty() ? std::string()
                                                       : kDot + std::to_string(s.selected_items.size()) + (s.selected_items.size() == 1 ? " item" : " items");
    ui::SectionOpts   so;
    so.foldable = false;
    so.suffix = count.empty() ? nullptr : count.c_str();
    if (ui::BeginSection(sec, "Selection", so)) {
        static const char* const kGest[2] = {"Attach", "Add to stack"};
        int                      gi = v.gc.gesture == am::Gesture::Stack ? 1 : 0;
        if (ui::Segmented("##amgest", kGest, 2, &gi, 0.0f, v.gc.stack_ok || gi == 1 ? -1 : 1))
            g.want = gi == 1 ? am::GestureWant::Stack : am::GestureWant::Attach;
        if (ui::SegmentedHovered() >= 0)
            ui::ShowTip(v.gc.stack_ok ? "Attach: the selected items onto the marker. Add to stack: onto the stack of the selected attached sound"
                                      : "Add to stack needs an attached sound selected with the new items");
        // What will happen.
        std::string bold, rest;
        if (v.gc.reason.empty()) {
            if (v.gc.gesture == am::Gesture::Stack) {
                bold = std::to_string(s.fresh) + (s.fresh == 1 ? " new item" : " new items");
                rest = " \xE2\x86\x92 a new layer on the stack of " + v.target;
            } else if (v.marker >= 0) {
                bold = std::to_string(s.piles) + (s.piles == 1 ? " pile" : " piles");
                const std::string kind_side = v.kind + (g.side_only && v.side ? " " + std::string(1, v.side) : std::string());
                rest = kDot + std::string("the first moved onto ") + v.target + ", copies on the other " + kind_side + " markers";
            }
        }
        if (!bold.empty()) {
            ImGui::PushFont(ui::FontSmall());
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
            ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::kMuted));
            ImGui::TextWrapped("%s%s", bold.c_str(), rest.c_str());
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
            ImGui::PopFont();
        }
        const float tcol = std::max(ImGui::CalcTextSize("Scope").x, ImGui::CalcTextSize("Side").x);
        {
            const float       fw = FieldLabel("Scope", tcol);
            const std::string all = "All with " + v.kind;
            const char*       kScope[2] = {"This animation", all.c_str()};
            int               si = g.scope_all ? 1 : 0;
            if (ui::Segmented("##amscope", kScope, 2, &si, fw * 0.5f)) {
                g.scope_all = si == 1;
                SavePrefFloat(kPrefScope, g.scope_all ? 1.0f : 0.0f);
            }
            if (ui::SegmentedHovered() >= 0)
                ui::ShowTip(("This animation: " + (v.anim.empty() ? std::string("the marker's animation") : v.anim) +
                             " and its linked copies. All with " + v.kind + ": every animation with a " + v.kind + " marker")
                                .c_str());
        }
        {
            const float       fw = FieldLabel("Side", tcol);
            const std::string one = (v.side ? std::string(1, v.side) : std::string("Side")) + " only";
            const char*       kSide[2] = {"Both sides", one.c_str()};
            int               si = g.side_only && v.side ? 1 : 0;
            if (ui::Segmented("##amside", kSide, 2, &si, fw * 0.5f, v.side ? -1 : 1)) {
                g.side_only = si == 1;
                SavePrefFloat(kPrefSide, g.side_only ? 1.0f : 0.0f);
            }
            if (ui::SegmentedHovered() >= 0)
                ui::ShowTip(v.side ? (v.kind + " " + std::string(1, v.side) + " markers only (RAV - Side Switch)").c_str()
                                   : "This marker kind has no side");
        }
        {  // 13-68e v4: what the other side becomes; always shown (greyed on Both sides) so the button never moves
            const char        od = v.side == 'R' ? 'L' : 'R';
            const std::string lbl = v.side ? std::string(1, od) : std::string("Other side");
            const bool        on = g.side_only && v.side;
            const float       fw = FieldLabel(lbl.c_str(), std::max(tcol, ImGui::CalcTextSize(lbl.c_str()).x));
            static const char* const kOther[2] = {"Keeps its sound", "Plays nothing"};
            int                      oi = g.other_none ? 1 : 0;
            if (!on) ImGui::BeginDisabled();
            if (ui::Segmented("##amother", kOther, 2, &oi, fw * 0.5f) && on) {
                g.other_none = oi == 1;
                SavePrefFloat(kPrefOther, g.other_none ? 1.0f : 0.0f);
            }
            if (!on) ImGui::EndDisabled();
            if (!on && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ui::ShowTip(!v.side ? "This marker kind has no side" : "With Both sides, nothing moves: pick the marker's side only first");
            else if (on && ui::SegmentedHovered() >= 0)
                ui::ShowTip(ui::SegmentedHovered() == 1 ? (lbl + " markers lose this library's sounds").c_str()
                                                        : (lbl + " markers keep what they play now").c_str());
        }
        if (!v.gc.reason.empty()) ui::ToneText(v.gc.reason, ui::kWarn, true, ImGui::GetContentRegionAvail().x);
        const std::string label = am::RunLabel(v.gc.gesture, g.scope_all, g.side_only, v.kind, v.side, g.other_none) + "##amrun";
        const bool        can = v.gc.reason.empty() && !g.pending;
        if (!can) ImGui::BeginDisabled();
        if (ui::PrimaryButton(label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, ui::LargeH())) && can) RunAttach(v);
        if (!can) ImGui::EndDisabled();
    }
    ui::EndSection(sec);
}

// ---- Detach tab ---------------------------------------------------------------------------------------------------

void DrawAsk()
{
    if (!g.ask_open) return;
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float  w = std::min(ui::Dp(290.0f), disp.x - ui::Dp(16.0f));
    ImGui::SetNextWindowSize(ImVec2(w, 0.0f));
    ImGui::SetNextWindowPos(ImVec2(std::max(ui::Dp(8.0f), std::min(g.ask.at.x - w + ui::Dp(24.0f), disp.x - w - ui::Dp(8.0f))), g.ask.at.y + ui::Dp(4.0f)),
                            ImGuiCond_Appearing);
    if (!ui::BeginPanelPopup("##amask", 0, ImVec2(10.0f, 10.0f))) {
        g.ask_open = false;
        g.ask_close = false;
        return;
    }
    if (g.ask_close) {  // a tab or mode change: the popup leaves the popup stack properly
        g.ask_close = false;
        g.ask_open = false;
        ImGui::CloseCurrentPopup();
        ui::EndPanelPopup();
        return;
    }
    ImGui::PushFont(ui::FontSemibold());
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(g.ask.title.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ui::ToneText(g.ask.sub, ui::kMuted, true, ImGui::GetContentRegionAvail().x);
    int        choice = 0;  // 1 keep, 2 delete, 3 cancel
    const bool busy = static_cast<bool>(g.pending);
    if (busy) ImGui::BeginDisabled();
    if (ui::SolidButton("Keep in place##amkeep")) choice = 1;
    ui::ItemTipText("The items stay where they are and no longer follow");
    ui::SameLineIfFits(ui::ToneButtonWidth("Delete"));
    if (ui::ToneButton("Delete##amdel", ui::TagTone::Bad)) choice = 2;
    if (busy) ImGui::EndDisabled();
    ui::SameLineIfFits(ui::ButtonWidth("Cancel"));
    if (ui::SolidButton("Cancel##amcancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) choice = 3;
    if (choice) {
        if (choice != 3) {
            const bool keep = choice == 1;
            const Ask  a = g.ask;
            if (a.type == 3) Queue([a, keep] { RemoveFromAttachedLibrary(a.lib, a.node, keep, a.side); });
            else Queue([a, keep] { DetachFromMarkers(a.targets, keep); });
            g.sel_row.clear();
        }
        g.ask_open = false;
        ImGui::CloseCurrentPopup();
    }
    ui::EndPanelPopup();
}

void DrawMarkerList(const am::ListFilter& f)
{
    const am::Snapshot&          s = g.st.snap;
    const char                   ds = DetachSide();
    const std::vector<am::AnimGroup> groups = am::MarkerRows(s, f, g.lib_pick, ds);
    int                          shown = 0;
    for (const am::AnimGroup& ag : groups) shown += static_cast<int>(ag.rows.size());
    bool any = false;
    for (const am::AttachedEvent& e : s.events) any |= (g.lib_pick.empty() || e.lib_uid == g.lib_pick) && (!ds || e.side == ds);
    const std::string title = g.lib_pick.empty() ? std::string("Attached") : "Attached on " + LibName(g.lib_pick);
    const std::string suffix = shown ? kDot + std::to_string(shown) + (shown == 1 ? " marker" : " markers") : std::string();
    ui::Section       sec;
    ui::SectionOpts   so;
    so.suffix = suffix.empty() ? nullptr : suffix.c_str();
    if (ui::BeginSection(sec, (title + "##amlist").c_str(), so)) {
        ImGui::PushFont(ui::FontSmall());
        ui::ToneText("Showing: " + f.label + (ds ? ", " + std::string(1, ds) + " markers" : std::string()), ui::kFaint, true,
                     ImGui::GetContentRegionAvail().x);
        ImGui::PopFont();
        if (!shown) {
            FaintLine(any ? "Nothing attached here." : "Nothing attached yet.");
        } else {
            std::vector<int> all;
            for (const am::AnimGroup& ag : groups) {
                const std::string asuf = kDot + std::to_string(ag.rows.size());
                ui::Section       as;
                ui::SectionOpts   ao;
                ao.suffix = asuf.c_str();
                const std::string guid = ag.anim >= 0 && static_cast<size_t>(ag.anim) < s.anims.size() ? s.anims[static_cast<size_t>(ag.anim)].guid : std::string();
                const std::string key = (ag.name.empty() ? std::string("Animation") : ag.name) + "##amanim" + guid;  // its fold stays its own
                const bool        open = ui::BeginSection(as, key.c_str(), ao);
                for (const am::MarkerRow& r : ag.rows) {
                    all.push_back(r.event);
                    if (!open) continue;
                    const am::AttachedEvent& e = s.events[static_cast<size_t>(r.event)];
                    const std::string        rk = "ev" + std::to_string(r.event);
                    ImVec2                   at;
                    const int                hit = ListRow(rk.c_str(), e.side ? std::string(1, e.side) : std::string("\xC2\xB7"), r.l1, r.l2,
                                                           g.sel_row == e.rid + "|" + std::to_string(e.t) + "|" + std::to_string(e.anim),
                                                           "Select in REAPER", "Detach from this marker", &at);
                    if (hit == 1) {
                        g.sel_row = e.rid + "|" + std::to_string(e.t) + "|" + std::to_string(e.anim);
                        SelectFromList(EventItems({r.event}), f);
                    } else if (hit == 2) {
                        Ask a;
                        a.type = 1;
                        a.targets = {TargetOf(e)};
                        int n = static_cast<int>(e.placed.size());
                        char b[32];
                        std::snprintf(b, sizeof(b), "%.2f s", e.pos);
                        a.title = "Detach from " + e.label + kDot + b;
                        a.sub = std::to_string(n) + (n == 1 ? " item" : " items") + ". The marker stays empty.";
                        a.at = at;
                        OpenAsk(a);
                    }
                }
                ui::EndSection(as);
            }
            const bool busy = static_cast<bool>(g.pending);
            if (busy) ImGui::BeginDisabled();
            if (ui::SolidButton("Select all##amselall")) {
                g.sel_row.clear();
                SelectFromList(EventItems(all), f);
            }
            ui::ItemTipText("Select every sound of the markers shown in REAPER");
            ImGui::SameLine();
            if (ui::SolidButton("Detach all##amdetall")) {
                Ask a;
                a.type = 2;
                int n = 0;
                for (int i : all) {
                    a.targets.push_back(TargetOf(s.events[static_cast<size_t>(i)]));
                    n += static_cast<int>(s.events[static_cast<size_t>(i)].placed.size());
                }
                a.title = "Detach all " + std::to_string(all.size()) + (all.size() == 1 ? " marker shown" : " markers shown");
                a.sub = std::to_string(n) + (n == 1 ? " item" : " items") + (all.size() == 1 ? ". The marker stays empty." : ". The markers stay empty.");
                a.at = ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
                OpenAsk(a);
            }
            ui::ItemTipText("Detach the sounds of every marker shown");
            if (busy) ImGui::EndDisabled();
        }
    }
    ui::EndSection(sec);
}

void DrawLibraryList(const std::vector<const SoundLibrary*>& libs)
{
    const SoundStore& store = GetSoundStore(nullptr);
    std::vector<const SoundLibrary*> shown;
    for (const SoundLibrary* l : libs)
        if (g.lib_pick.empty() || l->uid == g.lib_pick) shown.push_back(l);
    const char ds = DetachSide();  // 13-68e v4
    std::vector<std::pair<const SoundLibrary*, am::VariationRow>> rows;
    for (const SoundLibrary* l : shown)
        for (const am::VariationRow& r : am::VariationRows(store, *l, g.st.snap, ds)) rows.push_back({l, r});
    const std::string title = g.lib_pick.empty() ? std::string("Attached libraries") : LibName(g.lib_pick) + " library";
    const std::string suffix = rows.empty() ? std::string() : kDot + std::to_string(rows.size()) + (rows.size() == 1 ? " variation" : " variations");
    ui::Section       sec;
    ui::SectionOpts   so;
    so.suffix = suffix.empty() ? nullptr : suffix.c_str();
    if (ui::BeginSection(sec, (title + "##amlibs").c_str(), so)) {
        if (rows.empty()) {
            FaintLine(g.lib_pick.empty() ? "No Attached library yet." : "The " + LibName(g.lib_pick) + " library is empty.");
        } else {
            std::string              last;
            std::vector<std::string> all;
            for (size_t i = 0; i < rows.size(); ++i) {
                const SoundLibrary&     lib = *rows[i].first;
                const am::VariationRow& r = rows[i].second;
                if (shown.size() > 1 && lib.uid != last) {
                    ImGui::PushFont(ui::FontSmall());
                    ui::ToneText(lib.name, ui::kFaint);
                    ImGui::PopFont();
                }
                last = lib.uid;
                const std::vector<std::string> its = am::VariationItems(lib, r, g.st.snap, ds);  // 13-68e v4: that side's
                all.insert(all.end(), its.begin(), its.end());
                const std::string badge = r.kind == am::VariationKind::Layer ? "+" : r.kind == am::VariationKind::Takes ? std::to_string(r.takes) : "1";
                const std::string rk = "var" + std::to_string(i);
                ImVec2            at;
                const int         hit = ListRow(rk.c_str(), badge, r.name, am::VariationText(r), g.sel_row == lib.uid + "|" + r.node,
                                                "Select its placed sounds in REAPER", "Remove from the library", &at);
                if (hit == 1) {
                    g.sel_row = lib.uid + "|" + r.node;
                    SelectFromList(its, am::MakeFilter(g.st.snap));
                } else if (hit == 2) {
                    Ask a;
                    a.type = 3;
                    a.lib = lib.uid;
                    a.node = r.node;
                    const int n = static_cast<int>(its.size());
                    a.side = ds;
                    a.title = "Remove " + r.name + " from the " + lib.name + " library" +
                              (a.side ? " (" + std::string(1, a.side) + " only)" : std::string());
                    a.sub = std::to_string(n) + (n == 1 ? " placed sound." : " placed sounds.");
                    a.at = at;
                    OpenAsk(a);
                }
            }
            const bool busy = static_cast<bool>(g.pending);
            if (busy) ImGui::BeginDisabled();
            if (ui::SolidButton("Select all##amvselall")) {
                g.sel_row.clear();
                SelectFromList(all, am::MakeFilter(g.st.snap));
            }
            ui::ItemTipText("Select every sound placed from these libraries in REAPER");
            if (busy) ImGui::EndDisabled();
        }
    }
    ui::EndSection(sec);
}

void DrawDetach()
{
    const SoundStore&                store = GetSoundStore(nullptr);
    std::vector<const SoundLibrary*> libs;
    for (const SoundLibrary& l : store.libraries)
        if (l.attached) libs.push_back(&l);
    if (!g.lib_pick.empty() && !FindLibrary(store, g.lib_pick)) g.lib_pick.clear();
    const float col = std::max({ImGui::CalcTextSize("Library").x, ImGui::CalcTextSize("Detach").x, ImGui::CalcTextSize("Side").x});
    {
        const float       fw = FieldLabel("Library", col);
        const std::string preview = g.lib_pick.empty() ? std::string("All libraries") : LibName(g.lib_pick);
        if (libs.empty()) ImGui::BeginDisabled();
        if (ui::BeginComboStyled("##amlib", preview.c_str(), fw)) {
            if (ImGui::Selectable("All libraries", g.lib_pick.empty())) {
                g.lib_pick.clear();
                g.sel_row.clear();
            }
            for (const SoundLibrary* l : libs) {
                ImGui::PushID(l->uid.c_str());
                if (ImGui::Selectable(l->name.c_str(), g.lib_pick == l->uid)) {
                    g.lib_pick = l->uid;
                    g.sel_row.clear();
                }
                ImGui::PopID();
            }
            ui::EndComboStyled();
        }
        if (libs.empty()) ImGui::EndDisabled();
        ui::ItemTipText("The Attached libraries (one per marker kind)");
    }
    {
        const float              fw = FieldLabel("Detach", col);
        static const char* const kMode[2] = {"This marker", "Whole library"};
        int                      m = g.mode;
        if (ui::Segmented("##ammode", kMode, 2, &m, fw * 0.5f)) {
            g.mode = m;
            g.sel_row.clear();
            CloseAsk();
        }
        if (ui::SegmentedHovered() >= 0)
            ui::ShowTip(ui::SegmentedHovered() == 1 ? "The variation leaves the library: every sound placed from it"
                                                    : "Only the sounds on that marker; the marker then stays empty");
    }
    {  // 13-68e v4: Both / L / R, remembered
        const float              fw = FieldLabel("Side", col);
        static const char* const kSide[3] = {"Both", "L", "R"};
        const bool               sided = DetachSided();
        int                      d = sided ? g.dside : 0;  // the choice stays remembered; shown as Both meanwhile
        if (!sided) ImGui::BeginDisabled();
        const bool picked = ui::Segmented("##amdside", kSide, 3, &d, std::min(fw / 3.0f, ui::Dp(70.0f))) && sided;
        if (!sided) ImGui::EndDisabled();
        if (!sided && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ui::ShowTip(g.lib_pick.empty() ? "No Attached library has markers with a side" : "This library's markers have no side");
        if (picked) {
            g.dside = d;
            SavePrefFloat(kPrefDetachSide, static_cast<float>(d));
            g.sel_row.clear();
            g.frozen = false;
            CloseAsk();
        }
        if (sided && ui::SegmentedHovered() >= 0)
            ui::ShowTip(ui::SegmentedHovered() == 0 ? "Both sides' markers"
                                                    : (std::string(ui::SegmentedHovered() == 1 ? "L" : "R") +
                                                       " markers only; Whole library removes a variation from that side only")
                                                          .c_str());
    }
    ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
    if (g.mode == 1) {
        DrawLibraryList(libs);
    } else {
        const am::ListFilter f = g.frozen ? g.frozen_filter : am::MakeFilter(g.st.snap);
        DrawMarkerList(f);
    }
}

// ---- The window -------------------------------------------------------------------------------------------------

void DrawContent()
{
    // Esc closes the window, unless a popup (the Keep / Delete question, a dropdown) was open to take it.
    const bool popup_before = g.ask_open || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    Refresh();
    const AttachView v = MakeAttachView();
    if (g.tab == 1) {
        DrawTitle("Detach", g.lib_pick.empty() ? std::string("all libraries") : LibName(g.lib_pick));
    } else {
        DrawTitle(v.gc.gesture == am::Gesture::Stack ? "Add to stack" : "Attach", v.kind);
    }
    DrawTabs();
    const float h = std::max(ui::Dp(60.0f), ImGui::GetContentRegionAvail().y);
    if (ImGui::BeginChild("##ambody", ImVec2(0.0f, h), ImGuiChildFlags_None)) {
        if (g.tab == 1) DrawDetach();
        else DrawAttach(v);
        if (g.ask_open && !ImGui::IsPopupOpen("##amask")) ImGui::OpenPopup("##amask");
        DrawAsk();
    }
    ImGui::EndChild();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !popup_before) g.open = false;
}

}  // namespace

void OpenAttachManager(AttachManagerTab tab)
{
    ReadPrefs();
    g.tab = tab == AttachManagerTab::Detach ? 1 : 0;
    g.open = true;
    g.focus = true;
    g.place = true;
    g.dirty = true;
    g.force = true;
    CloseAsk();
    g.frozen = false;
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
    if (g.place && g.open) g_host.PlaceAtMouse();
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
    g.ask_open = false;
    g.ask_close = false;
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
