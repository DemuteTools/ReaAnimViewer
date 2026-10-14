// SPDX-License-Identifier: MIT
//
// See shortcuts_ui.h. The popup lists every key action (double-click = record a new key,
// right-click = back to the default), 13-45's "View: scroll and zoom" rows (REAPER | Own) and every
// mouse gesture (fixed, greyed). 13-60: every editable row can also follow a REAPER action the user
// picks (a key row's chain button, a view row's "Action"): the Link modal.

#include "shortcuts_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "action_link.h"      // 13-60
#include "library_view_keys.h" // 13-60 review: the keys the Library view uses
#include "reaper_actions.h"   // 13-60: REAPER's actions (names, the list)
#include "reaper_view_keys.h" // 13-60: an action's gestures in reaper-kb.ini
#include "shortcuts.h"
#include "ui_theme.h"
#include "view_nav.h"  // 13-45: the "View: scroll and zoom" rows

namespace rav {
namespace {

constexpr char kPopupId[]  = "##shortcuts";
constexpr char kRecordId[] = "##recordkey";

// Mockup px (Dp at use).
constexpr float kIconSize = 18.0f;   // the glyph
constexpr float kIconPad  = 3.0f;    // around it: a 24 px button
constexpr float kRowW     = 350.0f;  // the popup's rows
constexpr float kRowH     = 26.0f;

bool g_open          = false;  // the popup was open last frame
bool g_open_at_press = false;  // ...when the icon was pressed (a second click closes it)
bool g_close_request = false;  // Esc from the viewer's key handling
int  g_reset_refused = -1;     // 10-11 review: the action whose restore was refused (or -1)
int  g_reset_blocker = -1;     // ...the action that holds its default as your own key

// The fallback glyph when the icon texture is missing: a keyboard outline, keys, space bar.
void DrawKeyboardGlyph(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    const float w = s * 0.84f, h = s * 0.56f;
    const ImVec2 a(c.x - w * 0.5f, c.y - h * 0.5f), b(c.x + w * 0.5f, c.y + h * 0.5f);
    dl->AddRect(a, b, col, ui::Dp(2.0f), 0, ui::Dp(1.5f));
    const float k = w / 7.0f;
    for (int row = 0; row < 2; ++row)
        for (int i = 0; i < 5; ++i) {
            const ImVec2 p(a.x + k * (1.0f + static_cast<float>(i)), a.y + h * (0.22f + 0.24f * static_cast<float>(row)));
            dl->AddRectFilled(p, ImVec2(p.x + k * 0.6f, p.y + k * 0.6f), col);
        }
    dl->AddRectFilled(ImVec2(c.x - k * 1.5f, b.y - h * 0.3f), ImVec2(c.x + k * 1.5f, b.y - h * 0.3f + k * 0.55f), col);
}

void Header(const char* title, const char* note)
{
    ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
    ui::GroupCaption(title);
    if (note) {
        // The note (body font) on the caption's baseline (11 px small font).
        ImGui::SameLine();
        const ImVec2 c = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + ui::FontSmall()->Ascent - ImGui::GetFont()->Ascent));
        ui::ToneText(note, ui::kFaint);
    }
}

// The key column: caps right-aligned here, "fixed" right of them.
float CapRight(float row_right)
{
    return row_right - ui::Dp(10.0f) - ImGui::CalcTextSize("fixed").x - ui::Dp(10.0f);
}

// 13-60: the link a row asks for (the Link modal): a KEYS row's id, or kShortcutCount + a view row.
int  g_link_target = -1;
bool g_link_open_now = false;

void OpenLink(int target)
{
    CancelShortcutRecording();
    g_link_target = target;
    g_link_open_now = true;
}

// 13-60: REAPER's name of a linked action, looked up once per id while the popup stays open (it is
// looked up again when the popup opens: an extension loaded since names it).
struct LinkedName {
    std::string id;
    ReaperActionInfo info;
};
std::vector<LinkedName> g_names;

const ReaperActionInfo& LinkedActionInfo(const std::string& id)
{
    for (const LinkedName& n : g_names)
        if (n.id == id) return n.info;
    g_names.push_back(LinkedName{id, LookupReaperAction(id)});
    return g_names.back().info;
}

// 13-60: the line under a linked row: "⛓ <REAPER's name> · <id>" (faint), or why it is in doubt (warn).
void LinkLine(ImDrawList* dl, ImVec2 p, float right, const std::string& id)
{
    const ReaperActionInfo& info = LinkedActionInfo(id);
    std::string text = std::string("\xE2\x9B\x93 ");
    ImU32       col = ui::kFaint;
    if (!info.name.empty()) {
        text += info.name + " \xC2\xB7 " + id;
    } else if (info.cmd > 0) {
        text += id + " \xC2\xB7 REAPER gives it no name: is its extension or script loaded?";
        col = ui::kWarn;
    } else if (info.checked) {
        text += id + " \xC2\xB7 REAPER does not know it now";
        col = ui::kWarn;
    } else {
        text += id;
    }
    ImGui::PushFont(ui::FontSmall());
    const bool cut = ui::EllipsisText(dl, p, right, col, text);
    ImGui::PopFont();
    if (cut && ImGui::IsMouseHoveringRect(p, ImVec2(right, p.y + ui::Dp(16.0f))) && ImGui::IsWindowHovered())
        ImGui::SetTooltip("%s", text.c_str());
}

// 13-60: key caps right-aligned to `right`, as many as leave `left` free, then "+N" (all of them in its
// tooltip, on the row `hover`). Returns where they start.
struct Cap {
    std::string text;
    ImU32       col;
};
float DrawCaps(ImDrawList* dl, const std::vector<Cap>& caps, float left, float right, float y, bool hover)
{
    const float gap = ui::Dp(4.0f);
    auto        cap_w = [](const std::string& s) { return ImGui::CalcTextSize(s.c_str()).x + ui::Dp(8.0f); };
    size_t      shown = caps.size();
    char        more[16] = "";
    float       total = 0.0f;
    for (;;) {
        total = 0.0f;
        for (size_t i = 0; i < shown; ++i) total += cap_w(caps[i].text) + (i ? gap : 0.0f);
        if (shown < caps.size()) {
            std::snprintf(more, sizeof(more), "+%d", static_cast<int>(caps.size() - shown));
            total += gap + cap_w(more);
        }
        if (total <= right - left || shown <= 1) break;
        --shown;
    }
    const float x0 = std::max(left, right - total);
    float       x = x0;
    dl->PushClipRect(ImVec2(x0, y - ui::Dp(4.0f)), ImVec2(right, y + ImGui::GetTextLineHeight() + ui::Dp(6.0f)), true);
    for (size_t i = 0; i < shown; ++i) x += ui::KeyCap(dl, ImVec2(x, y), caps[i].text.c_str(), caps[i].col) + gap;
    if (shown < caps.size()) ui::KeyCap(dl, ImVec2(x, y), more, ui::kMuted);
    dl->PopClipRect();
    if (shown < caps.size() && hover) {
        std::string all;
        for (const Cap& c : caps) all += (all.empty() ? "" : ", ") + c.text;
        ui::ItemTipText(all.c_str());
    }
    return x0;
}

// 13-60: the small chain-link button of a row (lit while it is linked to an action the user picked):
// opens the Link modal. 13-60b: on every row, active in Own, greyed in REAPER (a click does nothing).
bool ChainButton(const char* id, ImVec2 c, bool on, bool enabled, bool takes_wheel)
{
    const float s = ui::Dp(20.0f);
    ImGui::SetCursorScreenPos(ImVec2(c.x - s * 0.5f, c.y - s * 0.5f));
    const bool  pressed = ImGui::InvisibleButton(id, ImVec2(s, s));
    const bool  hot = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hot && enabled) dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ui::kRaised, ui::Dp(4.0f));
    ImU32 col = on ? ui::kAccent : (hot ? ui::kText : ui::kFaint);
    if (!enabled) col = (ui::kFaint & 0x00FFFFFFu) | 0x70000000u;  // greyed
    // Two links, slanted: rounded outlines overlapping in the middle.
    const float w = s * 0.42f, h = s * 0.26f, r = h * 0.5f, t = ui::Dp(1.4f);
    dl->AddRect(ImVec2(c.x - w + ui::Dp(1.0f), c.y - h * 0.5f + ui::Dp(2.0f)), ImVec2(c.x + ui::Dp(1.0f), c.y + h * 0.5f + ui::Dp(2.0f)), col, r, 0, t);
    dl->AddRect(ImVec2(c.x - ui::Dp(1.0f), c.y - h * 0.5f - ui::Dp(2.0f)), ImVec2(c.x + w - ui::Dp(1.0f), c.y + h * 0.5f - ui::Dp(2.0f)), col, r, 0, t);
    if (hot) {
        if (!enabled)
            ui::ItemTipText("Switch to Own to link this row to a REAPER action of your choice.");
        else if (on)
            ui::ItemTipText("Linked to a REAPER action: click to pick another, or unlink it. Right-click the row: back to "
                            "its default. A faint key is another RAV action's: that one runs it.");
        else
            ui::ItemTipText(takes_wheel ? "Link to a REAPER action of your choice: RAV runs this on that action's keys and "
                                          "wheel in REAPER, followed live."
                                        : "Link to a REAPER action of your choice: RAV runs this on that action's keys in "
                                          "REAPER, followed live.");
    }
    return pressed && enabled;
}

// 13-60b -- every editable row (KEYS and VIEW) is drawn the same: line 1, the label, the gestures that
// run it now (key caps, whatever the mode) and the chain in the right column (where the mouse rows say
// "fixed"); line 2, the REAPER | Own switch and what the row follows ("⛓ <REAPER's name> · <id>", or
// "RAV only"). Double-click = record an own key / gesture (it makes the row Own), right-click = the
// row's default. The caller acts on what was clicked.
struct EditRowView {
    const char*      label = "";
    const char*      context = nullptr;  // "Video" / "Tagging", faint after the label
    std::vector<Cap> caps;
    bool             reaper = false;      // the switch's state
    bool             linked = false;      // Own, linked to an action the user picked (the chain lit)
    bool             takes_wheel = false;
    std::string      link_id;             // the action named on line 2 (looked up): REAPER's own, or the picked one
    std::string      source;              // ...else this text (faint)
    std::string      reaper_tip, own_tip;
};

enum class RowClick { None, Record, Reset, Reaper, Own, Chain };

RowClick EditRow(int imgui_id, bool recording, const EditRowView& v)
{
    const float line_h = ui::Dp(kRowH);
    const float sub_h = ui::Dp(18.0f);
    RowClick    click = RowClick::None;
    ImGui::PushID(imgui_id);
    ImGui::SetNextItemAllowOverlap();  // the switch and the chain sit on the row
    // On the popup's kRaised the row's hover is kHover (kRowHoverStrong).
    ui::SelectRow("##row", recording, ui::kRowHoverStrong, line_h + sub_h,
                  ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_NoAutoClosePopups | ImGuiSelectableFlags_AllowOverlap);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) click = RowClick::Record;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) click = RowClick::Reset;
    const bool   row_hover = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  th = ImGui::GetTextLineHeight();
    const float  cy = a.y + line_h * 0.5f;

    // Line 1: the gestures right-aligned (as many as leave the label some room, then "+N"), the label
    // giving way to them.
    const float lw = std::min(ImGui::CalcTextSize(v.label).x, ui::Dp(90.0f));
    const float cap_x = DrawCaps(dl, v.caps, a.x + ui::Dp(8.0f) + lw + ui::Dp(6.0f), CapRight(b.x), cy - th * 0.5f - ui::Dp(1.5f), row_hover);
    dl->PushClipRect(a, ImVec2(std::max(a.x, cap_x - ui::Dp(6.0f)), b.y), true);
    dl->AddText(ImVec2(a.x + ui::Dp(8.0f), cy - th * 0.5f), ui::kText, v.label);
    if (v.context)
        dl->AddText(ImVec2(a.x + ui::Dp(8.0f) + ImGui::CalcTextSize(v.label).x + ui::Dp(6.0f), cy - th * 0.5f), ui::kFaint, v.context);
    dl->PopClipRect();
    const float fw = ImGui::CalcTextSize("fixed").x;
    if (ChainButton("##link", ImVec2(b.x - ui::Dp(10.0f) - fw * 0.5f, cy), v.linked, !v.reaper, v.takes_wheel)) click = RowClick::Chain;

    // Line 2: the REAPER | Own switch (a pill of two small buttons), then what the row follows.
    ImFont*           sf = ui::FontSmall();
    const float       seg_h = ui::Dp(16.0f);
    const float       sy = a.y + line_h + std::floor((sub_h - seg_h) * 0.5f) - ui::Dp(3.0f);
    float             sx = a.x + ui::Dp(8.0f);
    const char* const segs[2] = {"REAPER", "Own"};
    const int         mode = v.reaper ? 0 : 1;
    float             seg_w[2];
    float             pill_w = 0.0f;
    for (int i = 0; i < 2; ++i) pill_w += (seg_w[i] = ui::TextSizeIn(sf, segs[i]).x + ui::Dp(14.0f));
    dl->AddRect(ImVec2(sx, sy), ImVec2(sx + pill_w, sy + seg_h), ui::kStroke, seg_h * 0.5f);
    for (int i = 0; i < 2; ++i) {
        const bool on = i == mode;
        ImGui::SetCursorScreenPos(ImVec2(sx, sy));
        if (ImGui::InvisibleButton(i == 0 ? "##reaper" : "##own", ImVec2(seg_w[i], seg_h)) && !on)
            click = i == 0 ? RowClick::Reaper : RowClick::Own;
        const bool hot = ImGui::IsItemHovered();
        if (hot) ui::ItemTipText(i == 0 ? v.reaper_tip.c_str() : v.own_tip.c_str());
        if (on)
            dl->AddRectFilled(ImVec2(sx + 1.0f, sy + 1.0f), ImVec2(sx + seg_w[i] - 1.0f, sy + seg_h - 1.0f),
                              i == 1 ? ui::kRaised : ui::kAccentSoft, seg_h * 0.5f);
        const ImU32  ink = on ? (i == 1 ? ui::kText : ui::kAccentText) : (hot ? ui::kText : ui::kFaint);
        const ImVec2 ts = ui::TextSizeIn(sf, segs[i]);
        ui::AddTextIn(dl, sf, ImVec2(sx + std::floor((seg_w[i] - ts.x) * 0.5f), sy + std::floor((seg_h - ts.y) * 0.5f)), ink, segs[i]);
        sx += seg_w[i];
    }
    if (!v.link_id.empty()) {
        LinkLine(dl, ImVec2(sx + ui::Dp(6.0f), sy + std::floor((seg_h - ui::TextSizeIn(sf, "A").y) * 0.5f)), b.x - ui::Dp(8.0f), v.link_id);
    } else {
        ImGui::PushFont(sf);
        const ImVec2 tp(sx + ui::Dp(6.0f), sy + std::floor((seg_h - ImGui::GetTextLineHeight()) * 0.5f));
        const bool   cut = ui::EllipsisText(dl, tp, b.x - ui::Dp(8.0f), ui::kFaint, v.source);
        ImGui::PopFont();
        if (cut && ImGui::IsMouseHoveringRect(ImVec2(tp.x, sy), ImVec2(b.x, sy + seg_h)) && ImGui::IsWindowHovered())
            ui::ShowTip(v.source.c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopID();
    return click;
}

// One key action. Returns true when a double-click asked to record it. 13-60b: REAPER = the keys REAPER
// gives RAV's own action for it (ShortcutDef::action, set in REAPER's Actions window); Own = its own key,
// or an action the user picked (the chain).
bool KeyRow(int id)
{
    const ShortcutDef& def = kShortcutTable[id];
    const bool         reaper = ShortcutFollowsReaper(id);
    const std::string  link = ShortcutLink(id);
    const bool         custom = !reaper && link.empty() && CurrentShortcuts()[static_cast<size_t>(id)] != def.def;
    EditRowView        v;
    v.label = def.label;
    if (def.context == ShortcutContext::VideoView) v.context = "Video";
    else if (def.context == ShortcutContext::TaggingView) v.context = "Tagging";
    else if (def.context == ShortcutContext::LibraryEdit) v.context = "Sound Edit";
    // The keys in force (REAPER's or a linked action's; a key another row takes, faint).
    const ShortcutKeymap& km = CurrentShortcutKeymap();
    for (const KeyBinding& k : km.keys[static_cast<size_t>(id)])
        v.caps.push_back(Cap{ShortcutBindingLabel(k), custom ? ui::kAccent : ui::kText});
    for (const KeyBinding& k : km.shadowed[static_cast<size_t>(id)]) v.caps.push_back(Cap{ShortcutBindingLabel(k), ui::kFaint});
    if (v.caps.empty()) v.caps.push_back((reaper || !link.empty()) ? Cap{"no shortcut in REAPER", ui::kFaint} : Cap{"None", ui::kText});  // 10-11: unbound
    v.reaper = reaper;
    v.linked = !reaper && !link.empty();
    v.link_id = reaper ? ShortcutReaperActionId(id) : link;
    if (v.link_id.empty()) {
        if (def.reaper_cmd)  // 13-61: its default is REAPER's action
            v.source = std::string("RAV only \xC2\xB7 default: REAPER's ") + def.reaper_cmd;
        else
            v.source = std::string("RAV only") + (custom ? " \xC2\xB7 default " + FormatShortcut(def.def, &LayoutKeyName) : std::string());
    }
    if (def.reaper_cmd) {  // 13-61: one of REAPER's own actions (the default)
        v.reaper_tip = std::string("Follow REAPER: your keys for \"") + (def.reaper_name ? def.reaper_name : def.reaper_cmd) +
                       "\" (" + def.reaper_cmd + "), read live: they do this in the Sound Edit window while it has the "
                       "focus, and still trim REAPER's items everywhere else (the default)";
        v.own_tip = "RAV's own key for the Sound Edit window only (double-click the row to record one), or a REAPER "
                    "action you pick with the chain";
    } else {
        v.reaper_tip = std::string("Follow REAPER: the keys you give \"") + def.action_desc +
                       "\" in REAPER's Actions list, read live (REAPER runs it too, outside the viewer). A key you give "
                       "the RAV action in REAPER is no longer REAPER's in the main window (one action per key)";
        v.own_tip = "RAV's own key (double-click the row to record one), or a REAPER action you pick with the chain";
    }
    bool record = false;
    switch (EditRow(id, ShortcutRecordingId() == id, v)) {
    case RowClick::Record: record = true; break;
    case RowClick::Reset:
        if (ShortcutDefaultFollowsReaper(id)) {  // 13-61: its default is REAPER
            SetShortcutFollowsReaper(id);
            break;
        }
        [[fallthrough]];
    case RowClick::Own: {
        // 10-11 review: an action without a key cannot take back a default that is your own key for
        // another action: refused, and the popup says why. 13-60: a linked row is unlinked; 13-60b: a
        // row on REAPER is Own again (its default key).
        const int blocker = reaper ? SetShortcutOwn(id) : ResetShortcutToDefault(id);
        g_reset_refused = blocker >= 0 ? id : -1;
        g_reset_blocker = blocker;
        break;
    }
    case RowClick::Reaper: SetShortcutFollowsReaper(id); break;
    case RowClick::Chain: OpenLink(id); break;
    case RowClick::None: break;
    }
    return record;
}

// 13-45 -- one "View: scroll and zoom" row (the mockup's option A). 13-60b: drawn as a KEYS row. REAPER =
// the user's binding of REAPER's matching action (kViewNavTable); Own = his own gesture for RAV only, or
// an action he picks (the chain). Double-click = record an own gesture, right-click = REAPER again.
// Returns true when a double-click asked to record.
bool NavRow(int row)
{
    const ViewNavDef&   def = kViewNavTable[row];
    const ViewNavChoice ch = CurrentViewNavChoices()[static_cast<size_t>(row)];
    const bool          reaper = ch.linked_default();
    const bool          linked = !ch.own && !ch.cmd.empty();
    EditRowView         v;
    v.label = def.label;
    for (const ViewGesture& g : ViewNavGestures(CurrentViewNav(), row))
        v.caps.push_back(Cap{ViewGestureLabel(g), ch.own ? ui::kAccent : ui::kText});
    if (v.caps.empty())
        v.caps.push_back(Cap{ch.own ? "double-click to set" : (reaper ? "not bound in REAPER" : "no shortcut in REAPER"), ui::kFaint});
    v.reaper = reaper;
    v.linked = linked;
    v.takes_wheel = def.wheel_ok;
    if (linked) {
        v.link_id = ch.cmd;
    } else if (reaper) {
        v.source = std::string("\xE2\x9B\x93 ") + def.reaper_name + " \xC2\xB7 " + std::to_string(def.reaper_cmd);
    } else {
        std::string re;
        for (const ViewGesture& g : ReaperViewGestures(CurrentReaperViewKeys(), row)) re += (re.empty() ? "" : ", ") + ViewGestureLabel(g);
        v.source = "RAV only \xC2\xB7 REAPER's would be " + (re.empty() ? std::string("none") : re);
    }
    v.reaper_tip = std::string("Follow REAPER: your binding of \"") + def.reaper_name + "\", read live";
    v.own_tip = std::string("Your own ") + (def.wheel_ok ? "key or wheel" : "key") +
                " for RAV only (double-click the row to record it), or a REAPER action you pick with the chain";
    bool record = false;
    switch (EditRow(1000 + row, ViewNavRecordingRow() == row, v)) {
    case RowClick::Record: record = true; break;
    case RowClick::Reset:
    case RowClick::Reaper: SetViewNavLinked(row); break;
    case RowClick::Own: SetViewNavOwn(row); break;
    case RowClick::Chain: OpenLink(kShortcutCount + row); break;
    case RowClick::None: break;
    }
    return record;
}

// 13-45: the record modal of a "View: scroll and zoom" row: a key or the wheel; a gesture another row
// runs waits for Keep (it is then taken from that row).
void NavRecordBody(int row)
{
    const ViewNavDef& def = kViewNavTable[row];
    ImGui::Text(def.wheel_ok ? "Press a key or turn the wheel for %s..." : "Press a key for %s...", def.label);
    ui::Caption("Ctrl, Shift and Alt can be held with it. Esc to cancel.");
    const float wrap = ui::Dp(320.0f);
    ViewGesture g;
    int         owner = -1;
    const bool  pending = ViewNavPending(&g, &owner);
    std::string label;
    if (pending) {
        label = ViewGestureLabel(g);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  w = ui::KeyCap(ImGui::GetWindowDrawList(), p, label.c_str(), ui::kAccent);
        ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight() + ui::Dp(3.0f)));
        const ViewNavChoice* oc = owner >= 0 ? &CurrentViewNavChoices()[static_cast<size_t>(owner)] : nullptr;
        const char* how = !oc ? "" : oc->linked_default() ? " (on REAPER)" : (!oc->own ? " (linked to a REAPER action)" : " (your own)");
        ui::ToneText(std::string("Already used by ") + (owner >= 0 ? kViewNavTable[owner].label : "another action") + how +
                         ". Keeping it takes it from that one.",
                     ui::kWarn, /*wrap=*/true, wrap);
    } else if (ViewNavRefusal()[0]) {
        ui::ToneText(ViewNavRefusal(), ui::kWarn, /*wrap=*/true, wrap);
    } else {
        ImGui::TextUnformatted(" ");  // keeps the modal's size when a warning shows
    }
    ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
    if (ui::SolidButton("Cancel", ImVec2(ui::Dp(90.0f), 0.0f))) {
        CancelShortcutRecording();
        ImGui::CloseCurrentPopup();
    }
    if (pending) {
        ImGui::SameLine();
        if (ui::SolidButton(("Keep " + label + "##navkeep").c_str())) ViewNavKeepPending();  // ends the recording
    }
}

// The group's note: where linked rows read REAPER's bindings, and when.
std::string NavKbNote()
{
    const double age = ViewNavKbAge();
    if (age < 0.0) return "REAPER and linked rows follow REAPER: no reaper-kb.ini found, so REAPER's own list of each action's shortcuts.";
    char when[48];
    if (age < 60.0) std::snprintf(when, sizeof(when), "%d s ago", static_cast<int>(age));
    else if (age < 3600.0) std::snprintf(when, sizeof(when), "%d min ago", static_cast<int>(age / 60.0));
    else std::snprintf(when, sizeof(when), "%d h ago", static_cast<int>(age / 3600.0));
    return std::string("REAPER and linked rows follow REAPER: read from reaper-kb.ini ") + when +
           " (again when REAPER saves it) and from REAPER's own list of each action's shortcuts.";
}

// One mouse gesture: read-only, greyed, "fixed".
void GestureRow(const char* action, const char* gesture)
{
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(ui::Dp(kRowW), ui::Dp(kRowH)));
    const float th = ImGui::GetTextLineHeight();
    const float cy = a.y + ui::Dp(kRowH) * 0.5f;
    const float right = a.x + ui::Dp(kRowW);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(a.x + ui::Dp(8.0f), cy - th * 0.5f), ui::kMuted, action);
    const float cap_w = ImGui::CalcTextSize(gesture).x + ui::Dp(8.0f);
    ui::KeyCap(dl, ImVec2(CapRight(right) - cap_w, cy - th * 0.5f - ui::Dp(1.5f)), gesture, ui::kMuted);
    const float fw = ImGui::CalcTextSize("fixed").x;
    dl->AddText(ImVec2(right - ui::Dp(10.0f) - fw, cy - th * 0.5f), ui::kFaint, "fixed");
}

void Note(const char* text)
{
    ui::ToneText(text, ui::kFaint, /*wrap=*/true, ui::Dp(kRowW));
}

void RecordModal(bool open_now)
{
    if (open_now) ImGui::OpenPopup(kRecordId);
    if (ui::BeginModalCard(kRecordId)) {
        const int id = ShortcutRecordingId();
        if (id < 0) {
            ImGui::CloseCurrentPopup();  // a key was taken, or Esc
        } else if (id >= kShortcutCount) {
            NavRecordBody(id - kShortcutCount);  // 13-45: a view row
        } else {
            ImGui::Text("Press a key for %s...", kShortcutTable[id].label);
            ui::Caption("Ctrl, Shift and Alt can be held with it. Esc to cancel.");
            if (const char* owner = ShortcutRecordingConflict()) {
                ui::ToneText(std::string("Already used by ") + owner, ui::kWarn);
            } else {
                ImGui::TextUnformatted(" ");  // keeps the modal's size when the warning shows
            }
            ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
            if (ui::SolidButton("Cancel", ImVec2(ui::Dp(90.0f), 0.0f))) {
                CancelShortcutRecording();
                ImGui::CloseCurrentPopup();
            }
        }
        ui::EndModalCard();
    }
}

// ---- 13-60: the Link modal: a row linked to any REAPER action -------------------------------------
//
// One field: paste a command ID (REAPER's Actions list: right-click > Copy selected action command ID)
// or type words of an action's name; the list under it (REAPER's Main section) narrows as you type, a
// click puts the action's id in the field. The id is checked with REAPER (its name, or why it is in
// doubt) and its gestures in reaper-kb.ini shown before Link.

constexpr char kLinkId[] = "##linkaction";

char                           g_link_buf[256] = "";
std::vector<ReaperActionEntry> g_actions;    // REAPER's Main section, read when the modal opens
std::vector<int>               g_hits;       // the entries the field matches
std::string                    g_hits_for;   // ...for this text
std::string                    g_typed_for;  // the text g_typed was looked up for
ReaperActionInfo               g_typed;
bool                           g_link_focus = false;
// The picked action's gestures, for this id and this read of reaper-kb.ini.
std::string                    g_preview_id;
int                            g_preview_gen = -1;
std::vector<KbGesture>         g_preview;
std::vector<std::string>       g_preview_listed;
int                            g_preview_unread = 0;  // listed shortcuts that are no key or wheel (MIDI, OSC...)
bool                           g_preview_known = false;

const char* LinkTargetLabel(int t)
{
    if (t >= 0 && t < kShortcutCount) return kShortcutTable[t].label;
    if (t >= kShortcutCount && t < kShortcutCount + kNavRowCount) return kViewNavTable[t - kShortcutCount].label;
    return "";
}

void Refilter()
{
    g_hits.clear();
    g_hits_for = g_link_buf;
    for (int i = 0; i < static_cast<int>(g_actions.size()); ++i)
        if (ActionSearchMatch(g_actions[static_cast<size_t>(i)].name, g_actions[static_cast<size_t>(i)].id, g_hits_for))
            g_hits.push_back(i);
}

void StartLink()
{
    const int t = g_link_target;
    std::string cur;
    if (t >= 0 && t < kShortcutCount) cur = ShortcutLink(t);
    else if (t >= kShortcutCount && t < kShortcutCount + kNavRowCount)
        cur = CurrentViewNavChoices()[static_cast<size_t>(t - kShortcutCount)].cmd;
    std::snprintf(g_link_buf, sizeof(g_link_buf), "%s", cur.c_str());
    g_actions = ListReaperActions();
    g_typed_for.clear();
    g_typed = ReaperActionInfo{};
    if (LooksLikeCommandId(g_link_buf)) {
        g_typed = LookupReaperAction(g_link_buf);
        g_typed_for = g_link_buf;
    }
    g_link_focus = true;
    g_preview_gen = -1;
    // The list starts whole (the current id in the field is no search).
    g_hits.clear();
    for (int i = 0; i < static_cast<int>(g_actions.size()); ++i) g_hits.push_back(i);
    g_hits_for = g_link_buf;
}

// The gestures of the action that the target would run on: a KEYS row takes keys only, Zoom in / out
// too; the other view rows keys and wheels.
bool TargetTakesWheel(int t)
{
    return t >= kShortcutCount && kViewNavTable[t - kShortcutCount].wheel_ok;
}

void LinkModal(bool open_now)
{
    if (open_now) {
        StartLink();
        ImGui::OpenPopup(kLinkId);
    }
    if (!ui::BeginModalCard(kLinkId, 0, ui::Dp(460.0f))) return;
    const int t = g_link_target;
    if (t < 0 || g_close_request) {  // Esc (the viewer's key handling): this modal only
        g_link_target = -1;
        g_close_request = false;
        ImGui::CloseCurrentPopup();
        ui::EndModalCard();
        return;
    }
    const float wrap = ImGui::GetContentRegionAvail().x;
    ui::ToneText(std::string("Link \"") + LinkTargetLabel(t) + "\" to a REAPER action", ui::kText, /*wrap=*/true, wrap);
    ui::ToneText(std::string("RAV then runs it on that action's ") + (TargetTakesWheel(t) ? "keys and wheel" : "keys") +
                     " in REAPER, followed live. Paste its command ID (REAPER's Actions list: right-click "
                     "an action > Copy selected action command ID), or type words of its name.",
                 ui::kMuted, /*wrap=*/true, wrap);
    ImGui::Dummy(ImVec2(0.0f, ui::Dp(2.0f)));
    if (g_link_focus) {
        ImGui::SetKeyboardFocusHere();
        g_link_focus = false;
    }
    const bool enter = ui::TextField("##linkcmd", "Command ID (990, _WOL_ADJENVTRHMOUSE) or words of its name", g_link_buf,
                                     sizeof(g_link_buf), 0.0f, ImGuiInputTextFlags_EnterReturnsTrue);  // Enter = Link
    if (g_hits_for != g_link_buf) Refilter();

    // What the field names, when it reads as an id.
    const std::string typed = g_link_buf;
    const bool        is_id = LooksLikeCommandId(typed);
    if (is_id && typed != g_typed_for) {
        g_typed = LookupReaperAction(typed);
        g_typed_for = typed;
    }
    bool ok = false;
    if (!is_id) {
        ui::ToneText(typed.empty() ? "No action picked yet." : "Pick an action in the list (or type its command ID).", ui::kFaint);
    } else if (g_typed.cmd > 0 && !g_typed.name.empty()) {
        ui::ToneText(std::string("\xE2\x9C\x93 ") + g_typed.name + "  \xC2\xB7  " + g_typed.id, ui::kOk, true, wrap);
        ok = true;
    } else if (g_typed.cmd > 0) {
        ui::ToneText(g_typed.id + ": REAPER knows this id but gives it no name, so its extension or script may not be "
                         "loaded now (a toolbar or menu can keep an id alive). It can be linked: it works once REAPER "
                         "has it again.",
                     ui::kWarn, true, wrap);
        ok = true;
    } else if (!g_typed.checked) {
        ui::ToneText(g_typed.id + ": REAPER cannot check ids here; it is linked as typed.", ui::kWarn, true, wrap);
        ok = !g_typed.id.empty();
    } else {
        ui::ToneText(std::string("REAPER has no action ") + g_typed.id + ".", ui::kBad, true, wrap);
    }

    // Its gestures (reaper-kb.ini, and the keys REAPER lists for it: what RAV will follow), and what
    // REAPER lists beyond them.
    if (ok) {
        if (g_preview_id != g_typed.id || g_preview_gen != ReaperKbGeneration()) {
            g_preview_id = g_typed.id;
            g_preview_gen = ReaperKbGeneration();
            g_preview = KbGesturesOf(CurrentReaperViewKeys(), g_typed.id);
            g_preview_listed = ReaperActionShortcuts(g_typed.cmd, &g_preview_known);
            g_preview_unread = 0;
            for (const std::string& d : g_preview_listed) {
                KbGesture g;
                if (!ParseReaperShortcutDesc(d, &g)) ++g_preview_unread;
                else if (std::find(g_preview.begin(), g_preview.end(), g) == g_preview.end()) g_preview.push_back(g);
            }
        }
        const std::vector<KbGesture>& gs = g_preview;
        std::string                   got, skipped;
        for (const KbGesture& g : gs) {
            const std::string s = ViewGestureLabel(ViewGestureOf(g));
            std::string& into = (g.input == KbInput::Key || TargetTakesWheel(t)) ? got : skipped;
            into += (into.empty() ? "" : ", ") + s;
        }
        ui::ToneText(got.empty() ? std::string("It has no ") + (TargetTakesWheel(t) ? "key or wheel" : "key") +
                                       " in REAPER yet: give it one in REAPER's Actions list, RAV follows it live."
                                 : "RAV will run on: " + got,
                     got.empty() ? ui::kWarn : ui::kText, true, wrap);
        if (!skipped.empty())
            ui::ToneText(std::string("Not used here (") + (t < kShortcutCount ? "a key action" : LinkTargetLabel(t)) +
                             " takes keys only): " + skipped,
                         ui::kFaint, true, wrap);
        // Some of what REAPER lists is no key or wheel RAV can read (MIDI, OSC...): say what it lists.
        if (g_preview_known && g_preview_unread > 0) {
            std::string all;
            for (const std::string& d : g_preview_listed) all += (all.empty() ? "" : ", ") + d;
            ui::ToneText("REAPER lists " + all + ". RAV follows its keys and wheels; MIDI, OSC, trackpad gestures and the "
                             "Win key are not followed.",
                         ui::kFaint, true, wrap);
        }
        // A KEYS row: a key another RAV action holds stays that one's (a character binding too: the key
        // typing it here); a key the Library view uses stays the Library's while it has the focus.
        if (t < kShortcutCount) {
            std::string held, library;
            for (const KbGesture& g : gs) {
                KeyBinding kb;
                if (!KeyBindingOfGesture(g, &kb)) continue;
                const int o = ShortcutKeyOwner(CurrentShortcutKeymap(), kb, t);
                if (o >= 0 && ShortcutFollowedAction(o).empty())
                    held += (held.empty() ? "" : ", ") + ShortcutBindingLabel(kb) + " (\"" + kShortcutTable[o].label + "\")";
                if (LibraryViewUsesKey(kb.vk, kb.ctrl, kb.shift, kb.alt))
                    library += (library.empty() ? "" : ", ") + ShortcutBindingLabel(kb);
            }
            if (!held.empty()) ui::ToneText("Keys RAV already gives another action keep running it: " + held, ui::kWarn, true, wrap);
            if (!library.empty())
                ui::ToneText("The Library view uses " + library + " itself: while it has the focus, " +
                                 (library.find(',') == std::string::npos ? "that key does" : "those keys do") +
                                 " what the Library does, not this action.",
                             ui::kWarn, true, wrap);
        }
    }

    // The list of REAPER's actions.
    ImGui::Dummy(ImVec2(0.0f, ui::Dp(2.0f)));
    const float list_h = ui::Dp(200.0f);
    if (g_actions.empty()) {
        ui::ToneText("REAPER's action list cannot be read here: paste the command ID.", ui::kFaint, true, wrap);
    } else {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(ui::kBg));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(ui::kStroke));
        const bool shown = ImGui::BeginChild("##actionlist", ImVec2(0.0f, list_h), ImGuiChildFlags_Borders);
        ImGui::PopStyleColor(2);
        if (shown) {
            ImGuiListClipper clip;
            clip.Begin(static_cast<int>(g_hits.size()), ui::Dp(22.0f));
            while (clip.Step()) {
                for (int k = clip.DisplayStart; k < clip.DisplayEnd; ++k) {
                    const ReaperActionEntry& e = g_actions[static_cast<size_t>(g_hits[static_cast<size_t>(k)])];
                    ImGui::PushID(k);
                    const bool sel = is_id && SameCommandId(e.id, g_typed.id);
                    if (ui::SelectRow("##act", sel, ui::kRowHoverStrong, ui::Dp(22.0f),
                                      ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_NoAutoClosePopups)) {
                        std::snprintf(g_link_buf, sizeof(g_link_buf), "%s", e.id.c_str());
                        g_hits_for = g_link_buf;  // the list stays as it was
                    }
                    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                    ImDrawList*  dl = ImGui::GetWindowDrawList();
                    ImFont*      sf = ui::FontSmall();
                    const ImVec2 ids = ui::TextSizeIn(sf, e.id.c_str());
                    const float  id_x = std::max(a.x + ui::Dp(80.0f), b.x - ui::Dp(6.0f) - std::min(ids.x, ui::Dp(160.0f)));
                    const float  th = ImGui::GetTextLineHeight();
                    ui::EllipsisText(dl, ImVec2(a.x + ui::Dp(6.0f), (a.y + b.y - th) * 0.5f), id_x - ui::Dp(8.0f), ui::kText, e.name);
                    ImGui::PushFont(sf);
                    ui::EllipsisText(dl, ImVec2(id_x, (a.y + b.y - ImGui::GetTextLineHeight()) * 0.5f), b.x - ui::Dp(6.0f), ui::kFaint, e.id);
                    ImGui::PopFont();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ui::ItemTipText((e.name + "\n" + e.id).c_str());
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndChild();
    }
    char count[48];
    std::snprintf(count, sizeof(count), "%d of %d actions", static_cast<int>(g_hits.size()), static_cast<int>(g_actions.size()));
    if (!g_actions.empty()) ui::ToneText(count, ui::kFaint);

    ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
    if (ui::SolidButton("Cancel", ImVec2(ui::Dp(90.0f), 0.0f))) {
        g_link_target = -1;
        ImGui::CloseCurrentPopup();
    }
    // 13-60b: a linked row can be unlinked here: Own, its default key (KEYS) / no gesture yet (VIEW).
    const bool linked_now = t < kShortcutCount ? !ShortcutLink(t).empty()
                                               : !CurrentViewNavChoices()[static_cast<size_t>(t - kShortcutCount)].cmd.empty();
    if (linked_now) {
        ImGui::SameLine();
        if (ui::SolidButton("Unlink", ImVec2(ui::Dp(90.0f), 0.0f))) {
            if (t < kShortcutCount) {
                const int blocker = ResetShortcutToDefault(t);
                g_reset_refused = blocker >= 0 ? t : -1;
                g_reset_blocker = blocker;
            } else {
                SetViewNavOwn(t - kShortcutCount);
            }
            g_link_target = -1;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered())
            ui::ItemTipText(t < kShortcutCount ? "Back to its own key (its default)" : "Back to an own gesture (double-click the row to record it)");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!ok);
    if ((ui::PrimaryButton("Link", ImVec2(ui::Dp(90.0f), 0.0f)) || enter) && ok) {
        if (t < kShortcutCount) LinkShortcutToAction(t, g_typed.id);
        else SetViewNavAction(t - kShortcutCount, g_typed.id);
        g_names.clear();  // its name is looked up again
        g_link_target = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ui::EndModalCard();
}

void PopupBody()
{
    ImGui::TextUnformatted("Shortcuts");
    Note("Every row: REAPER follows REAPER, live (a key: the keys you give its \"RAV: Viewer key\" action in REAPER's "
         "Actions list; a view row: your binding of REAPER's matching action). Own: RAV's own, double-click to record "
         "it, or the chain: a REAPER action of your choice. Right-click: back to the default. Keys work while the "
         "viewer has the focus.");

    Header("KEYS", nullptr);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::Dp(8.0f), ui::Dp(2.0f)));
    int record = -1;
    for (int i = 0; i < kShortcutCount; ++i)
        if (kShortcutTable[i].context != ShortcutContext::LibraryEdit && KeyRow(i)) record = i;
    ImGui::PopStyleVar();
    // 13-61: the Sound Edit window's trims, on REAPER's own item trims by default.
    Header("KEYS: SOUND EDIT", "Library \xE2\x80\xBA Edit window");
    Note("On REAPER, your keys for REAPER's own item trims trim the variations while the Sound Edit window has the "
         "focus. The edit cursor is where you last clicked the waveform; the zone is a drag across it.");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::Dp(8.0f), ui::Dp(2.0f)));
    for (int i = 0; i < kShortcutCount; ++i)
        if (kShortcutTable[i].context == ShortcutContext::LibraryEdit && KeyRow(i)) record = i;
    ImGui::PopStyleVar();
    if (record >= 0) {
        BeginShortcutRecording(record);
        g_reset_refused = -1;
    }
    // A refused restore stays explained while that action still has no key.
    if (g_reset_refused >= 0 && CurrentShortcuts()[static_cast<size_t>(g_reset_refused)].vk == 0 && g_reset_blocker >= 0 &&
        ShortcutFollowedAction(g_reset_refused).empty()) {
        const std::string def = FormatShortcut(kShortcutTable[g_reset_refused].def);
        ui::ToneText(std::string("\"") + kShortcutTable[g_reset_refused].label + "\" cannot get " + def +
                         " back: it is your key for \"" + kShortcutTable[g_reset_blocker].label +
                         "\". Give that action another key first, or double-click to pick another key.",
                     ui::kWarn, /*wrap=*/true, ui::Dp(kRowW));
    } else {
        g_reset_refused = -1;
    }

    // 13-45: the six view actions of every timeline (the mockup's option A).
    Header("VIEW: SCROLL AND ZOOM", "every timeline in RAV");
    Note("Sound Edit, Blend lanes, the Video and Tagging strips. Own can also take a REAPER action of your choice "
         "with the chain (e.g. _WOL_ADJENVTRHMOUSE).");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::Dp(8.0f), ui::Dp(2.0f)));
    int nav_record = -1;
    for (int r = 0; r < kNavRowCount; ++r)
        if (NavRow(r)) nav_record = r;
    ImGui::PopStyleVar();
    Note(NavKbNote().c_str());
    if (nav_record >= 0) {
        CancelShortcutRecording();
        BeginViewNavRecording(nav_record);
        g_reset_refused = -1;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::Dp(8.0f), ui::Dp(2.0f)));
    constexpr int kGroups = static_cast<int>(sizeof(kMouseGestureGroups) / sizeof(kMouseGestureGroups[0]));
    for (int g = 0; g < kGroups; ++g) {
        const MouseGestureGroup& grp = kMouseGestureGroups[g];
        Header(grp.title, grp.note);
        for (const MouseGestureDef& m : kMouseGestureTable)
            if (m.group == g) GestureRow(m.action, m.gesture);
        if (grp.footer) Note(grp.footer);
    }
    ImGui::PopStyleVar();

    RecordModal(record >= 0 || nav_record >= 0);
    LinkModal(std::exchange(g_link_open_now, false));  // 13-60
}

}  // namespace

void DrawShortcutsButton(float right_x, float centre_y, ImTextureID icon)
{
    const float btn = ui::Dp(kIconSize + 2.0f * kIconPad);
    const bool  shown = ui::BeginOverlay("##keysbtn", ImVec2(right_x, centre_y - btn * 0.5f), ImVec2(1.0f, 0.0f));
    if (shown) {
        // Frameless like the menu button; lit while the popup is open. No tooltip: the
        // popup is the help.
        ui::IconOnlyOpts o;
        o.w = btn;
        o.on = g_open;
        o.icon_px = ui::Dp(kIconSize);
        const bool pressed = ui::IconOnlyButton("##keys", icon, nullptr, o);
        if (!icon) {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const ImU32  tint = (g_open || ImGui::IsItemHovered()) ? ui::kText : ui::kMuted;
            DrawKeyboardGlyph(ImGui::GetWindowDrawList(), ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f),
                              ui::Dp(kIconSize), tint);
        }
        // A click outside the popup closes it on the press; a press on the icon while it
        // was open must not reopen it on the release.
        if (ImGui::IsItemActivated()) g_open_at_press = g_open;
        if (pressed && !g_open_at_press) {
            ImGui::OpenPopup(kPopupId);
            g_names.clear();  // 13-60: linked actions' names looked up again (an extension loaded since)
            g_link_target = -1;
            RefreshReaperListedKeys();  // ...and the keys REAPER lists for them
        }
        const ImVec2 bmax = ImGui::GetItemRectMax();

        ImGui::SetNextWindowPos(ImVec2(bmax.x, bmax.y + ui::Dp(6.0f)), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        // 13-60b: every row is two lines now: below the button, it scrolls rather than run off the viewer.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const float          room = vp->WorkPos.y + vp->WorkSize.y - (bmax.y + ui::Dp(6.0f)) - ui::Dp(8.0f);
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, std::max(room, ui::Dp(160.0f))));
        const bool open = ui::BeginPanelPopup(kPopupId);
        if (open) {
            // 13-60: Esc in the Link modal closes that modal only (LinkModal takes the request).
            if (g_close_request && ShortcutRecordingId() < 0 && g_link_target < 0) ImGui::CloseCurrentPopup();
            PopupBody();
            ui::EndPanelPopup();
        }
        g_open = open;
    } else {
        g_open = false;
    }
    g_close_request = false;
    ui::EndOverlay();

    // The popup went away (e.g. the viewer closed it): no recording without its modal.
    if (!g_open && ShortcutRecordingId() >= 0) CancelShortcutRecording();
}

bool ShortcutsPopupOpen()
{
    return g_open;
}

void RequestCloseShortcutsPopup()
{
    g_close_request = true;
}

}  // namespace rav

#endif  // _WIN32
