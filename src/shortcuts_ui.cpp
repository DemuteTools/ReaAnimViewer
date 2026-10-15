// SPDX-License-Identifier: MIT
//
// See shortcuts_ui.h. 13-64 (mockup maquette-fenetre-raccourcis-13-64.html): the Shortcuts window is a
// large modal sheet over the viewer. Left, its sections (All, Viewer, Video, Tagging, Sound Edit,
// Timelines, Mouse); centre, one line per row in columns (Action, Where, Keys, Follows, Source, the
// chain) under a name search; right, the selected row's facts and its buttons (Record key..., Link...,
// Default). Find by key (top bar) lists what already uses a key or a wheel in REAPER's Main section and
// in RAV, and links a REAPER result to a row at once. The Link modal (13-60) finds an action by name,
// command ID or shortcut, typed or pressed. Rows, storage and behaviour are 13-60b's: a row is REAPER
// (it follows REAPER's keys for its action) or Own (its own key, or a REAPER action the user picked).

#include "shortcuts_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "action_link.h"         // 13-60
#include "library_view_keys.h"   // 13-60 review: the keys the Library view uses
#include "reaper_actions.h"      // 13-60: REAPER's actions (names, the list)
#include "reaper_view_keys.h"    // 13-60: an action's gestures in reaper-kb.ini
#include "shortcut_key_index.h"  // 13-64: what uses a key (Find by key, the Link search)
#include "shortcuts.h"
#include "ui_flow.h"
#include "ui_theme.h"
#include "view_nav.h"  // 13-45: the "Timelines" rows

namespace rav {
namespace {

constexpr char kSheetId[]   = "##shortcuts";
constexpr char kRecordId[]  = "##recordkey";
constexpr char kRowMenuId[] = "##rowmenu";
constexpr char kLinkId[]    = "##linkaction";

// Mockup px (Dp at use).
constexpr float kIconSize = 18.0f;  // the keyboard glyph
constexpr float kIconPad  = 3.0f;   // around it: a 24 px button
constexpr float kRowH     = 30.0f;  // a table row
constexpr float kNavW     = 170.0f;
constexpr float kSideW    = 290.0f;
constexpr float kTopH     = 44.0f;
constexpr float kFootH    = 24.0f;
constexpr float kGap      = 8.0f;   // between the table's columns

bool g_open          = false;  // the window was open last frame
bool g_open_at_press = false;  // ...when the icon was pressed (a second click closes it)
bool g_close_request = false;  // Esc from the viewer's key handling
int  g_reset_refused = -1;     // 10-11 review: the action whose restore was refused (or -1)
int  g_reset_blocker = -1;     // ...the action that holds its default as your own key

// ---- rows and sections --------------------------------------------------------------------------

enum class Sec : int { All, Viewer, Video, Tagging, Edit, Timelines, Mouse, Count };
const char* const kSecLabel[] = {"All", "Viewer", "Video", "Tagging", "Sound Edit", "Timelines", "Mouse"};

struct RowRef {
    enum class Kind { Key, Nav, Mouse };
    Kind kind = Kind::Key;
    int  index = 0;
    bool operator==(const RowRef& o) const { return kind == o.kind && index == o.index; }
    bool operator!=(const RowRef& o) const { return !(*this == o); }
};

constexpr int kMouseRowCount = static_cast<int>(sizeof(kMouseGestureTable) / sizeof(kMouseGestureTable[0]));

Sec SecOf(const RowRef& r)
{
    if (r.kind == RowRef::Kind::Nav) return Sec::Timelines;
    if (r.kind == RowRef::Kind::Mouse) return Sec::Mouse;
    switch (kShortcutTable[r.index].context) {
    case ShortcutContext::VideoView: return Sec::Video;
    case ShortcutContext::TaggingView: return Sec::Tagging;
    case ShortcutContext::LibraryEdit: return Sec::Edit;
    default: return Sec::Viewer;
    }
}

const char* WhereOf(const RowRef& r)
{
    if (r.kind == RowRef::Kind::Mouse) return kMouseGestureTable[r.index].group == 1 ? "Shot strip" : "Viewer";
    return kSecLabel[static_cast<int>(SecOf(r))];
}

const char* LabelOf(const RowRef& r)
{
    if (r.kind == RowRef::Kind::Key) return kShortcutTable[r.index].label;
    if (r.kind == RowRef::Kind::Nav) return kViewNavTable[r.index].label;
    return kMouseGestureTable[r.index].action;
}

// Every row, section by section (the table's order).
const std::vector<RowRef>& AllRows()
{
    static std::vector<RowRef> rows;
    if (!rows.empty()) return rows;
    for (int s = static_cast<int>(Sec::Viewer); s <= static_cast<int>(Sec::Mouse); ++s) {
        for (int i = 0; i < kShortcutCount; ++i)
            if (static_cast<int>(SecOf(RowRef{RowRef::Kind::Key, i})) == s) rows.push_back(RowRef{RowRef::Kind::Key, i});
        if (s == static_cast<int>(Sec::Timelines))
            for (int i = 0; i < kNavRowCount; ++i) rows.push_back(RowRef{RowRef::Kind::Nav, i});
        if (s == static_cast<int>(Sec::Mouse))
            for (int i = 0; i < kMouseRowCount; ++i) rows.push_back(RowRef{RowRef::Kind::Mouse, i});
    }
    return rows;
}

// The link target of a row (13-60: a KEYS row's id, or kShortcutCount + a view row; -1 = none).
int TargetOf(const RowRef& r)
{
    if (r.kind == RowRef::Kind::Key) return r.index;
    if (r.kind == RowRef::Kind::Nav) return kShortcutCount + r.index;
    return -1;
}

bool RowOnReaper(const RowRef& r)
{
    if (r.kind == RowRef::Kind::Key) return ShortcutFollowsReaper(r.index);
    if (r.kind == RowRef::Kind::Nav) return CurrentViewNavChoices()[static_cast<size_t>(r.index)].linked_default();
    return false;
}

// The REAPER action a row picked (Own, linked): "" = none.
std::string RowLink(const RowRef& r)
{
    if (r.kind == RowRef::Kind::Key) return ShortcutLink(r.index);
    if (r.kind == RowRef::Kind::Nav) {
        const ViewNavChoice& c = CurrentViewNavChoices()[static_cast<size_t>(r.index)];
        return c.own ? std::string() : c.cmd;
    }
    return std::string();
}

// ---- REAPER's actions: names, the reverse index ------------------------------------------------

// 13-60: REAPER's name of a linked action, looked up once per id while the window stays open (again when
// it opens: an extension loaded since names it).
struct LinkedName {
    std::string      id;
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

// A character gesture ("+") as the key typing it on this layout (the index and the captures compare keys).
bool NormalizeGesture(KbGesture* g)
{
    if (g->input != KbInput::Key || !g->is_char) return true;
    KeyBinding b;
    if (!KeyBindingOfGesture(*g, &b)) return false;
    *g = KbGestureOfBinding(b);
    return true;
}

std::string GestureLabel(const KbGesture& g)
{
    return ViewGestureLabel(ViewGestureOf(g));
}

KbGesture KbOfView(const ViewGesture& v)
{
    KbGesture g;
    g.mods = v.mods;
    if (v.kind == GestureKind::Wheel) g.input = KbInput::Wheel;
    else if (v.kind == GestureKind::HWheel) g.input = KbInput::HWheel;
    else {
        g.input = KbInput::Key;
        g.code = v.code;
        g.is_char = v.is_char;
    }
    NormalizeGesture(&g);
    return g;
}

// 13-64: every Main action with its keys, read when the window opens and again when REAPER's keys are
// read again (ReaperKbGeneration), never per frame (one ReaperActionShortcuts per action).
ShortcutKeyIndex g_index;
int              g_index_gen = -1;
bool             g_index_built = false;
int              g_index_serial = 0;  // moves on each build (the Link list's indices follow it)

void EnsureIndex()
{
    if (g_index_built && g_index_gen == ReaperKbGeneration()) return;
    std::vector<KeyIndexSource> src;
    for (const ReaperActionEntry& e : ListReaperActions()) {
        KeyIndexSource s;
        s.id = e.id;
        s.name = e.name;
        bool known = false;
        if (e.cmd > 0) s.shortcuts = ReaperActionShortcuts(e.cmd, &known);
        src.push_back(std::move(s));
    }
    g_index = BuildShortcutKeyIndex(src, &CurrentReaperViewKeys(), &NormalizeGesture);
    g_index_gen = ReaperKbGeneration();
    g_index_built = true;
    ++g_index_serial;
}

// ---- the window's state -----------------------------------------------------------------------

Sec    g_sec = Sec::All;
RowRef g_sel;
char   g_search[128] = "";

enum class Capture { None, Find, Link };
Capture   g_capture = Capture::None;
bool      g_capture_btn_hot = false;  // the button that armed the capture is under the mouse (this frame)
bool      g_found_on = false;  // Find by key has a result
KbGesture g_found;
int       g_found_char_row = -1;  // a view row the found key runs through a character binding

bool g_record_now = false;  // open the record modal this frame
int  g_link_target = -1;    // 13-60: the link a row asks for (TargetOf)
bool g_link_open_now = false;
bool   g_menu_now = false;
RowRef g_menu_row;

// D8: the toast after a Find by key link, with its undo.
std::string           g_toast;
double                g_toast_at = -1.0;
std::function<void()> g_toast_undo;

void Toast(const std::string& text, std::function<void()> undo)
{
    g_toast = text;
    g_toast_at = ImGui::GetTime();
    g_toast_undo = std::move(undo);
}

void OpenLink(int target)
{
    CancelShortcutRecording();
    g_capture = Capture::None;
    g_link_target = target;
    g_link_open_now = true;
}

// Record a new key / gesture for a row (double-click, the menu, the detail's button).
void StartRecord(const RowRef& r)
{
    g_capture = Capture::None;
    if (r.kind == RowRef::Kind::Key) {
        BeginShortcutRecording(r.index);
    } else if (r.kind == RowRef::Kind::Nav) {
        CancelShortcutRecording();
        BeginViewNavRecording(r.index);
    } else {
        return;
    }
    g_reset_refused = -1;
    g_record_now = true;
}

// 10-11 review: a row without a key cannot take back a default that is your own key for another row:
// refused, and the detail says why.
void NoteRefusal(int id, int blocker)
{
    g_reset_refused = blocker >= 0 ? id : -1;
    g_reset_blocker = blocker;
}

// Back to the row's default (right-click menu, the detail's Default).
void ResetRow(const RowRef& r)
{
    if (r.kind == RowRef::Kind::Key) {
        const int id = r.index;
        if (ShortcutDefaultFollowsReaper(id)) {  // 13-61: its default is REAPER
            SetShortcutFollowsReaper(id);
            return;
        }
        NoteRefusal(id, ShortcutFollowsReaper(id) ? SetShortcutOwn(id) : ResetShortcutToDefault(id));
    } else if (r.kind == RowRef::Kind::Nav) {
        SetViewNavLinked(r.index);
    }
}

// The REAPER | Own switch.
void SetRowMode(const RowRef& r, bool reaper)
{
    g_sel = r;  // a refusal shows in its detail
    if (r.kind == RowRef::Kind::Key) {
        const int id = r.index;
        if (reaper) SetShortcutFollowsReaper(id);
        else NoteRefusal(id, ShortcutFollowsReaper(id) ? SetShortcutOwn(id) : ResetShortcutToDefault(id));
    } else if (r.kind == RowRef::Kind::Nav) {
        if (reaper) SetViewNavLinked(r.index);
        else SetViewNavOwn(r.index);
    }
}

// ---- D8: a Find by key result linked to a row at once, and its undo -----------------------------

struct RowSnapshot {
    RowRef        r;
    bool          reaper = false;
    std::string   link;
    KeyBinding    own;
    ViewNavChoice nav;
};

RowSnapshot Snapshot(const RowRef& r)
{
    RowSnapshot s;
    s.r = r;
    if (r.kind == RowRef::Kind::Key) {
        s.reaper = ShortcutFollowsReaper(r.index);
        s.link = ShortcutLink(r.index);
        s.own = CurrentShortcuts()[static_cast<size_t>(r.index)];
    } else if (r.kind == RowRef::Kind::Nav) {
        s.nav = CurrentViewNavChoices()[static_cast<size_t>(r.index)];
    }
    return s;
}

// Puts a row back as it was, through the same paths as the window's own clicks (a key it had is
// recorded again; a key another row took since stays that row's).
void Restore(const RowSnapshot& s)
{
    if (s.r.kind == RowRef::Kind::Key) {
        const int id = s.r.index;
        if (s.reaper) {
            SetShortcutFollowsReaper(id);
        } else if (!s.link.empty()) {
            LinkShortcutToAction(id, s.link);
        } else if (s.own.vk == 0) {
            if (ShortcutDefaultFollowsReaper(id)) {
                SetShortcutFollowsReaper(id);
                SetShortcutOwn(id);
            } else {
                ResetShortcutToDefault(id);
            }
        } else {
            ResetShortcutToDefault(id);
            if (ShortcutFollowsReaper(id) || CurrentShortcuts()[static_cast<size_t>(id)] != s.own || !ShortcutLink(id).empty()) {
                BeginShortcutRecording(id);
                ShortcutRecordKey(s.own.vk, s.own.ctrl, s.own.shift, s.own.alt);
                if (ShortcutRecordingId() >= 0) CancelShortcutRecording();
            }
        }
    } else if (s.r.kind == RowRef::Kind::Nav) {
        const int row = s.r.index;
        if (s.nav.linked_default()) {
            SetViewNavLinked(row);
        } else if (!s.nav.own) {
            SetViewNavAction(row, s.nav.cmd);
        } else {
            SetViewNavLinked(row);
            SetViewNavOwn(row);
            const ViewGesture& g = s.nav.g;
            if (g.kind != GestureKind::None && !(g.kind == GestureKind::Key && g.is_char)) {
                BeginViewNavRecording(row);
                const bool ctrl = (g.mods & kViewModCtrl) != 0, shift = (g.mods & kViewModShift) != 0,
                           alt = (g.mods & kViewModAlt) != 0;
                if (g.kind == GestureKind::Key) ViewNavRecordKey(static_cast<unsigned>(g.code), ctrl, shift, alt);
                else ViewNavRecordWheel(g.kind == GestureKind::HWheel, ctrl, shift, alt);
                if (ViewNavRecordingRow() >= 0) CancelViewNavRecording();
            }
        }
    }
    g_names.clear();
}

void LinkFromFind(const RowRef& r, const std::string& id, const std::string& name)
{
    const RowSnapshot before = Snapshot(r);
    if (r.kind == RowRef::Kind::Key) LinkShortcutToAction(r.index, id);
    else if (r.kind == RowRef::Kind::Nav) SetViewNavAction(r.index, id);
    else return;
    g_names.clear();
    g_sel = r;
    if (g_sec != Sec::All && g_sec != SecOf(r)) g_sec = Sec::All;
    Toast(std::string(LabelOf(r)) + " linked to " + (name.empty() ? id : name), [before]() { Restore(before); });
}

// ---- drawing helpers -----------------------------------------------------------------------------

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

// One key cap of a row: its text, ink, the gesture it stands for (the Find result lights it) and a
// one-line tip.
struct Cap {
    std::string text;
    ImU32       col = 0;
    bool        has_g = false;
    KbGesture   g;
    std::string tip;
};

bool CapHit(const Cap& c)
{
    return g_found_on && c.has_g && c.g == g_found;
}

// A Sound Edit key the Edit window itself takes (Space, Shift+Space, Esc, E, a view key) never runs a row.
bool EditWindowTakes(const KeyBinding& k)
{
    if (ViewNavRowForKeyDown(k.vk, k.ctrl, k.shift, k.alt) >= 0) return true;
    const int mods = KbGestureOfBinding(k).mods;
    for (const EditReservedKey& r : kEditReservedKeys)
        if (r.vk == k.vk && r.mods == mods) return true;
    return false;
}

std::vector<Cap> CapsOf(const RowRef& r)
{
    std::vector<Cap> caps;
    if (r.kind == RowRef::Kind::Mouse) {
        Cap c;
        c.text = kMouseGestureTable[r.index].gesture;
        c.col = ui::kMuted;
        c.has_g = ParseReaperShortcutDesc(c.text, &c.g) && c.g.input != KbInput::Key;
        caps.push_back(c);
        return caps;
    }
    if (r.kind == RowRef::Kind::Nav) {
        const ViewNavChoice ch = CurrentViewNavChoices()[static_cast<size_t>(r.index)];
        for (const ViewGesture& g : ViewNavGestures(CurrentViewNav(), r.index)) {
            Cap c;
            c.text = ViewGestureLabel(g);
            c.col = ch.own ? ui::kAccent : ui::kText;
            c.has_g = true;
            c.g = KbOfView(g);
            caps.push_back(c);
        }
        return caps;
    }
    const int          id = r.index;
    const ShortcutDef& def = kShortcutTable[id];
    const bool         reaper = ShortcutFollowsReaper(id);
    const bool         custom = !reaper && ShortcutLink(id).empty() && CurrentShortcuts()[static_cast<size_t>(id)] != def.def;
    const bool         fallback = reaper && ShortcutOnReaperFallback(id);
    const ShortcutKeymap& km = CurrentShortcutKeymap();
    for (const KeyBinding& k : km.keys[static_cast<size_t>(id)]) {
        Cap c;
        c.text = ShortcutBindingLabel(k);
        c.has_g = true;
        c.g = KbGestureOfBinding(k);
        const bool lost = def.context == ShortcutContext::LibraryEdit && EditWindowTakes(k);
        c.col = lost ? ui::kFaint : (custom ? ui::kAccent : ui::kText);
        if (lost) c.tip = c.text + " is taken by the Edit window";
        else if (fallback && k == def.reaper_fallback)
            c.tip = std::string("REAPER has no key for ") + def.reaper_cmd + ": " + c.text + " runs it";
        caps.push_back(c);
    }
    for (const KeyBinding& k : km.shadowed[static_cast<size_t>(id)]) {
        Cap c;
        c.text = ShortcutBindingLabel(k);
        c.col = ui::kFaint;
        c.has_g = true;
        c.g = KbGestureOfBinding(k);
        const int o = ShortcutKeyOwner(km, k, id);
        if (o >= 0) c.tip = c.text + " runs \"" + kShortcutTable[o].label + "\"";
        caps.push_back(c);
    }
    return caps;
}

// What a row with no gesture shows in its Keys column.
const char* NoCapsText(const RowRef& r)
{
    if (r.kind == RowRef::Kind::Nav) {
        const ViewNavChoice& ch = CurrentViewNavChoices()[static_cast<size_t>(r.index)];
        return ch.own ? "not set" : "none in REAPER";
    }
    return (ShortcutFollowsReaper(r.index) || !ShortcutLink(r.index).empty()) ? "none in REAPER" : "None";
}

// The caps from `left`, as many as fit before `right`, then "+N" (all of them in its tip). The cap under
// the mouse shows its own tip. Returns the width drawn.
float DrawCaps(ImDrawList* dl, const std::vector<Cap>& caps, float left, float right, float y, bool hover)
{
    const float gap = ui::Dp(4.0f);
    auto        cap_w = [](const std::string& s) { return ImGui::CalcTextSize(s.c_str()).x + ui::Dp(8.0f); };
    size_t      shown = caps.size();
    char        more[16] = "";
    for (;;) {
        float total = 0.0f;
        for (size_t i = 0; i < shown; ++i) total += cap_w(caps[i].text) + (i ? gap : 0.0f);
        if (shown < caps.size()) {
            std::snprintf(more, sizeof(more), "+%d", static_cast<int>(caps.size() - shown));
            total += gap + cap_w(more);
        }
        if (total <= right - left || shown <= 1) break;
        --shown;
    }
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float  th = ImGui::GetTextLineHeight() + ui::Dp(3.0f);
    float        x = left;
    dl->PushClipRect(ImVec2(left, y - ui::Dp(4.0f)), ImVec2(right, y + th + ui::Dp(4.0f)), true);
    const std::string* tip = nullptr;
    for (size_t i = 0; i < shown; ++i) {
        const Cap&  c = caps[i];
        const bool  hit = CapHit(c);
        const float w = ui::KeyCap(dl, ImVec2(x, y), c.text.c_str(), hit ? ui::kWarn : c.col);
        if (hit) dl->AddRect(ImVec2(x, y - ui::Dp(1.0f)), ImVec2(x + w, y + th), ui::kWarn, ui::Dp(4.0f));
        if (hover && mouse.x >= x && mouse.x < x + w && mouse.y >= y - ui::Dp(2.0f) && mouse.y < y + th && !c.tip.empty()) tip = &c.tip;
        x += w + gap;
    }
    if (shown < caps.size()) x += ui::KeyCap(dl, ImVec2(x, y), more, ui::kMuted) + gap;
    dl->PopClipRect();
    if (hover && tip) {
        ui::ShowTip(tip->c_str());
    } else if (hover && shown < caps.size() && mouse.x >= left && mouse.x < std::min(x, right)) {
        std::string all;
        for (const Cap& c : caps) all += (all.empty() ? "" : ", ") + c.text;
        ui::ShowTip(all.c_str());
    }
    return x - left;
}

// 13-60: the chain button of a row (lit while linked to an action the user picked): opens the Link modal.
// 13-60b: active in Own, greyed in REAPER (a click does nothing).
bool ChainButton(const char* id, ImVec2 c, bool on, bool enabled)
{
    const float s = ui::Dp(22.0f), g = ui::Dp(20.0f);
    ImGui::SetCursorScreenPos(ImVec2(c.x - s * 0.5f, c.y - s * 0.5f));
    const bool  pressed = ImGui::InvisibleButton(id, ImVec2(s, s));
    const bool  hot = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hot && enabled) dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ui::kRaised, ui::Dp(4.0f));
    ImU32 col = on ? ui::kAccent : (hot ? ui::kText : ui::kFaint);
    if (!enabled) col = ui::WithAlpha(ui::kFaint, 0x70);  // greyed
    // Two links, slanted: rounded outlines overlapping in the middle.
    const float w = g * 0.42f, h = g * 0.26f, r = h * 0.5f, t = ui::Dp(1.4f);
    dl->AddRect(ImVec2(c.x - w + ui::Dp(1.0f), c.y - h * 0.5f + ui::Dp(2.0f)), ImVec2(c.x + ui::Dp(1.0f), c.y + h * 0.5f + ui::Dp(2.0f)), col, r, 0, t);
    dl->AddRect(ImVec2(c.x - ui::Dp(1.0f), c.y - h * 0.5f - ui::Dp(2.0f)), ImVec2(c.x + w - ui::Dp(1.0f), c.y + h * 0.5f - ui::Dp(2.0f)), col, r, 0, t);
    if (hot) ui::ItemTipText(!enabled ? "Switch to Own to link" : (on ? "Linked: change or unlink" : "Link to a REAPER action"));
    return pressed && enabled;
}

// The REAPER | Own pill of a row (a mouse row: one "Fixed" segment). Returns 0 none, 1 REAPER, 2 Own.
int ModePill(ImDrawList* dl, ImVec2 p, const RowRef& r)
{
    ImFont*     sf = ui::FontSmall();
    const float seg_h = ui::Dp(20.0f);
    if (r.kind == RowRef::Kind::Mouse) {
        const float w = ui::TextSizeIn(sf, "Fixed").x + ui::Dp(16.0f);
        dl->AddRect(p, ImVec2(p.x + w, p.y + seg_h), ui::kStroke, seg_h * 0.5f);
        dl->AddRectFilled(ImVec2(p.x + 1.0f, p.y + 1.0f), ImVec2(p.x + w - 1.0f, p.y + seg_h - 1.0f), ui::kRaised, seg_h * 0.5f);
        const ImVec2 ts = ui::TextSizeIn(sf, "Fixed");
        ui::AddTextIn(dl, sf, ImVec2(p.x + std::floor((w - ts.x) * 0.5f), p.y + std::floor((seg_h - ts.y) * 0.5f)), ui::kMuted, "Fixed");
        return 0;
    }
    const bool        reaper = RowOnReaper(r);
    const char* const segs[2] = {"REAPER", "Own"};
    float             seg_w[2], pill_w = 0.0f;
    for (int i = 0; i < 2; ++i) pill_w += (seg_w[i] = ui::TextSizeIn(sf, segs[i]).x + ui::Dp(16.0f));
    dl->AddRect(p, ImVec2(p.x + pill_w, p.y + seg_h), ui::kStroke, seg_h * 0.5f);
    std::string reaper_tip;
    if (r.kind == RowRef::Kind::Key && kShortcutTable[r.index].reaper_cmd) {
        const ShortcutDef& def = kShortcutTable[r.index];
        reaper_tip = std::string("Use your REAPER keys for ") + def.reaper_cmd;
        if (def.reaper_fallback.vk != 0) reaper_tip += " (" + ShortcutBindingLabel(def.reaper_fallback) + " while it has none)";
    } else if (r.kind == RowRef::Kind::Nav) {
        reaper_tip = "Use your REAPER keys for " + std::to_string(kViewNavTable[r.index].reaper_cmd);
    } else {
        reaper_tip = "Use the keys REAPER gives this action";
    }
    int   click = 0;
    float x = p.x;
    for (int i = 0; i < 2; ++i) {
        const bool on = (i == 0) == reaper;
        ImGui::SetCursorScreenPos(ImVec2(x, p.y));
        if (ImGui::InvisibleButton(i == 0 ? "##reaper" : "##own", ImVec2(seg_w[i], seg_h)) && !on) click = i + 1;
        const bool hot = ImGui::IsItemHovered();
        if (hot) ui::ItemTipText(i == 0 ? reaper_tip.c_str() : "RAV's own key, or a linked REAPER action");
        if (on)
            dl->AddRectFilled(ImVec2(x + 1.0f, p.y + 1.0f), ImVec2(x + seg_w[i] - 1.0f, p.y + seg_h - 1.0f),
                              i == 1 ? ui::kRaised : ui::kAccentSoft, seg_h * 0.5f);
        const ImU32  ink = on ? (i == 1 ? ui::kText : ui::kAccentText) : (hot ? ui::kText : ui::kFaint);
        const ImVec2 ts = ui::TextSizeIn(sf, segs[i]);
        ui::AddTextIn(dl, sf, ImVec2(x + std::floor((seg_w[i] - ts.x) * 0.5f), p.y + std::floor((seg_h - ts.y) * 0.5f)), ink, segs[i]);
        x += seg_w[i];
    }
    return click;
}

// What a row follows, one line: "⛓ <REAPER's name> · <id>", "RAV only", "fixed". *col: its ink (warn when
// the linked id is in doubt).
std::string SourceOf(const RowRef& r, ImU32* col)
{
    *col = ui::kFaint;
    if (r.kind == RowRef::Kind::Mouse) return "fixed";
    std::string id;
    if (r.kind == RowRef::Kind::Key) {
        const int id_ = r.index;
        id = ShortcutFollowsReaper(id_) ? ShortcutReaperActionId(id_) : ShortcutLink(id_);
        if (id.empty()) {
            const ShortcutDef& def = kShortcutTable[id_];
            const bool custom = CurrentShortcuts()[static_cast<size_t>(id_)] != def.def;
            if (def.reaper_cmd) return std::string("RAV only \xC2\xB7 default: REAPER's ") + def.reaper_cmd;
            return std::string("RAV only") + (custom && def.def.vk ? " \xC2\xB7 default " + ShortcutBindingLabel(def.def) : std::string());
        }
    } else {
        const ViewNavChoice& ch = CurrentViewNavChoices()[static_cast<size_t>(r.index)];
        if (ch.linked_default())
            return std::string("\xE2\x9B\x93 ") + kViewNavTable[r.index].reaper_name + " \xC2\xB7 " +
                   std::to_string(kViewNavTable[r.index].reaper_cmd);
        if (ch.own) return "RAV only";
        id = ch.cmd;
    }
    const ReaperActionInfo& info = LinkedActionInfo(id);
    if (!info.name.empty()) return std::string("\xE2\x9B\x93 ") + info.name + " \xC2\xB7 " + id;
    if (info.cmd > 0 || info.checked) *col = ui::kWarn;
    return std::string("\xE2\x9B\x93 ") + id;
}

// ---- the table -------------------------------------------------------------------------------

struct Cols {
    float label = 0, where = 0, keys = 0, follow = 0, src = 0, chain = 0;  // x of each (from the row's left)
    float label_w = 0, where_w = 0, keys_w = 0, follow_w = 0, src_w = 0;
    bool  show_src = true;
};

Cols LayoutCols(float w)
{
    Cols        c;
    const float pad = ui::Dp(10.0f), gap = ui::Dp(kGap);
    const float where_w = ui::Dp(90.0f), follow_w = ui::Dp(118.0f), chain_w = ui::Dp(30.0f);
    const float inner = w - 2.0f * pad;
    float       flex = inner - where_w - follow_w - chain_w - 5.0f * gap;
    c.show_src = flex >= ui::Dp(150.0f + 140.0f + 120.0f);
    if (!c.show_src) flex += gap;
    const float fr = c.show_src ? 1.4f + 1.2f + 1.3f : 1.4f + 1.2f;
    flex = std::max(flex, ui::Dp(120.0f));
    c.label_w = flex * 1.4f / fr;
    c.keys_w = flex * 1.2f / fr;
    c.src_w = c.show_src ? flex * 1.3f / fr : 0.0f;
    c.where_w = where_w;
    c.follow_w = follow_w;
    c.label = pad;
    c.where = c.label + c.label_w + gap;
    c.keys = c.where + where_w + gap;
    c.follow = c.keys + c.keys_w + gap;
    c.src = c.follow + follow_w + gap;
    c.chain = (c.show_src ? c.src + c.src_w + gap : c.src) + chain_w * 0.5f;
    return c;
}

bool RowVisible(const RowRef& r)
{
    if (g_sec != Sec::All && SecOf(r) != g_sec) return false;
    return ActionSearchMatch(LabelOf(r), "", g_search);  // every word, any order
}

bool RowRecording(const RowRef& r)
{
    if (r.kind == RowRef::Kind::Key) return ShortcutRecordingId() == r.index;
    if (r.kind == RowRef::Kind::Nav) return ViewNavRecordingRow() == r.index;
    return false;
}

void DrawRow(const RowRef& r, const Cols& c, float w)
{
    const float h = ui::Dp(kRowH);
    ImGui::PushID(static_cast<int>(r.kind) * 1000 + r.index);
    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ui::SelectRow("##row", g_sel == r, ui::kRowHoverStrong, h,
                                       ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_NoAutoClosePopups |
                                           ImGuiSelectableFlags_AllowOverlap,
                                       ui::kRadiusSm, w);
    const bool   hover = ui::SelectRowHovered();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    if (clicked) g_sel = r;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) StartRecord(r);
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        g_sel = r;
        g_menu_row = r;
        g_menu_now = true;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (RowRecording(r)) dl->AddRect(a, b, ui::kAccent, ui::Dp(ui::kRadiusSm));
    const float th = ImGui::GetTextLineHeight();
    const float ty = a.y + (h - th) * 0.5f;
    const bool  mouse = r.kind == RowRef::Kind::Mouse;
    // Action, Where.
    ui::EllipsisText(dl, ImVec2(a.x + c.label, ty), a.x + c.label + c.label_w, mouse ? ui::kMuted : ui::kText, LabelOf(r));
    ImGui::PushFont(ui::FontSmall());
    ui::EllipsisText(dl, ImVec2(a.x + c.where, a.y + (h - ImGui::GetTextLineHeight()) * 0.5f), a.x + c.where + c.where_w,
                     ui::kFaint, WhereOf(r));
    ImGui::PopFont();
    // Keys.
    const std::vector<Cap> caps = CapsOf(r);
    if (caps.empty()) {
        ImGui::PushFont(ui::FontSmall());
        dl->AddText(ImVec2(a.x + c.keys, a.y + (h - ImGui::GetTextLineHeight()) * 0.5f), ui::kFaint, NoCapsText(r));
        ImGui::PopFont();
    } else {
        DrawCaps(dl, caps, a.x + c.keys, a.x + c.keys + c.keys_w, ty - ui::Dp(1.5f), hover);
    }
    // 13-62: the Solo / Mute rows say what they do over their keys.
    if (r.kind == RowRef::Kind::Key && (r.index == kShortcutEditSolo || r.index == kShortcutEditMute) && hover &&
        ImGui::GetIO().MousePos.x < a.x + c.keys)
        ui::ShowTip(r.index == kShortcutEditSolo ? "Solo the selected part (not saved)" : "Mute the selected part (not saved)");
    // Follows.
    const int mode = ModePill(dl, ImVec2(a.x + c.follow, a.y + (h - ui::Dp(20.0f)) * 0.5f), r);
    if (mode) SetRowMode(r, mode == 1);
    // Source.
    if (c.show_src) {
        ImU32             col = ui::kFaint;
        const std::string src = SourceOf(r, &col);
        ImGui::PushFont(ui::FontSmall());
        const ImVec2 sp(a.x + c.src, a.y + (h - ImGui::GetTextLineHeight()) * 0.5f);
        const bool   cut = ui::EllipsisText(dl, sp, a.x + c.src + c.src_w, col, src);
        ImGui::PopFont();
        if (cut && hover && ImGui::IsMouseHoveringRect(ImVec2(sp.x, a.y), ImVec2(a.x + c.src + c.src_w, b.y))) ui::ShowTip(src.c_str());
    }
    // The chain.
    if (!mouse && ChainButton("##link", ImVec2(a.x + c.chain, (a.y + b.y) * 0.5f), !RowLink(r).empty(), !RowOnReaper(r)))
        OpenLink(TargetOf(r));
    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopID();
}

void RowMenu()
{
    if (g_menu_now) {
        ImGui::OpenPopup(kRowMenuId);
        g_menu_now = false;
    }
    if (!ui::BeginMenuPopup(kRowMenuId)) return;
    if (g_close_request) {  // Esc: this menu only
        g_close_request = false;
        ImGui::CloseCurrentPopup();
    }
    const RowRef r = g_menu_row;
    if (r.kind == RowRef::Kind::Mouse) {
        ui::MenuEntry("Fixed gesture", nullptr, false, false);
    } else {
        const bool wheel = r.kind == RowRef::Kind::Nav && kViewNavTable[r.index].wheel_ok;
        if (ui::MenuEntry(wheel ? "Record key or wheel\xE2\x80\xA6" : "Record key\xE2\x80\xA6")) StartRecord(r);
        if (ui::MenuEntry("Link to REAPER action\xE2\x80\xA6", nullptr, false, !RowOnReaper(r))) OpenLink(TargetOf(r));
        if (ui::MenuEntry("Back to default")) ResetRow(r);
    }
    ui::EndMenuPopup();
}

// ---- Find by key ---------------------------------------------------------------------------------

// The dashed "Find by key" field of the top bar (and the Link modal's key button): idle dashed, armed
// accent, holding a gesture solid. Returns true when clicked.
bool CaptureButton(const char* id, const char* label, bool armed, bool has, float min_w, bool glyph)
{
    const float h = ui::FieldH();
    const float gw = glyph ? ui::Dp(22.0f) : 0.0f;
    const float w = std::max(min_w, ImGui::CalcTextSize(label).x + gw + ui::Dp(20.0f));
    const bool  pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool  hot = ImGui::IsItemHovered();
    if (armed && ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax())) g_capture_btn_hot = true;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = ui::Dp(ui::kRadiusMd);
    if (armed) {
        dl->AddRectFilled(a, b, ui::kAccentSoft, r);
        dl->AddRect(a, b, ui::kAccent, r);
    } else {
        dl->AddRectFilled(a, b, hot ? ui::kRaised : ui::kSurface, r);
        if (has || hot) dl->AddRect(a, b, ui::kStrokeStrong, r);
        else ui::DashedRect(dl, a, b, ui::kStrokeStrong, r);
    }
    const ImU32 ink = armed ? ui::kAccentText : (has ? ui::kText : ui::kMuted);
    float       x = a.x + ui::Dp(10.0f);
    if (glyph) {
        DrawKeyboardGlyph(dl, ImVec2(x + ui::Dp(8.0f), (a.y + b.y) * 0.5f), ui::Dp(16.0f), ink);
        x += gw;
    }
    dl->AddText(ImVec2(x, (a.y + b.y - ImGui::GetTextLineHeight()) * 0.5f), ink, label);
    return pressed;
}

const char* RavModeText(const RowRef& r)
{
    if (r.kind == RowRef::Kind::Mouse) return "fixed";
    if (RowOnReaper(r)) return "REAPER";
    return RowLink(r).empty() ? "Own" : "linked";
}

void FindPanel(float w)
{
    const std::vector<int>       reaper = g_index.ActionsOn(g_found);
    const std::vector<RavKeyUse> rav = RavUsesOf(CurrentShortcutKeymap(), CurrentViewNav(), g_found, g_found_char_row);
    const std::string            label = GestureLabel(g_found);
    ImDrawList*                  dl = ImGui::GetWindowDrawList();
    const ImVec2                 p0 = ImGui::GetCursorScreenPos();
    // Header: the key, how many uses, clear.
    ImGui::SetCursorScreenPos(ImVec2(p0.x + ui::Dp(10.0f), p0.y + ui::Dp(8.0f)));
    {
        const ImVec2 cp = ImGui::GetCursorScreenPos();
        const float  kw = ui::KeyCap(dl, ImVec2(cp.x, cp.y + ui::Dp(3.0f)), label.c_str(), ui::kWarn);
        dl->AddRect(ImVec2(cp.x, cp.y + ui::Dp(2.0f)), ImVec2(cp.x + kw, cp.y + ui::Dp(6.0f) + ImGui::GetTextLineHeight()), ui::kWarn, ui::Dp(4.0f));
        ImGui::Dummy(ImVec2(kw, ui::ControlH()));
        ImGui::SameLine();
        const int n = static_cast<int>(reaper.size() + rav.size());
        char      uses[32];
        std::snprintf(uses, sizeof(uses), "%d use%s", n, n == 1 ? "" : "s");
        ImGui::AlignTextToFramePadding();
        ui::ToneText(uses, ui::kMuted);
        ImGui::SetCursorScreenPos(ImVec2(p0.x + w - ui::Dp(10.0f) - ui::ControlH(), cp.y));
        if (ui::IconOnlyButton("##fkclear", "\xE2\x9C\x95", "Clear")) g_found_on = false;
    }
    const float row_h = ui::Dp(26.0f);
    const int   rows = static_cast<int>(reaper.size() + rav.size());
    const float list_h = rows == 0 ? ImGui::GetTextLineHeight() + ui::Dp(6.0f) : std::min(row_h * static_cast<float>(rows), ui::Dp(184.0f));
    ImGui::SetCursorScreenPos(ImVec2(p0.x + ui::Dp(10.0f), ImGui::GetCursorScreenPos().y + ui::Dp(4.0f)));
    const float inner = w - ui::Dp(20.0f);
    if (rows == 0) {
        ui::ToneText("Nothing uses " + label + " in REAPER (Main) or RAV.", ui::kFaint);
    } else if (ImGui::BeginChild("##fkrows", ImVec2(inner, list_h))) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const float sec_w = ui::Dp(110.0f), id_w = ui::Dp(130.0f), go_w = ui::Dp(200.0f);
        const float avail = ImGui::GetContentRegionAvail().x;
        const float name_w = std::max(ui::Dp(80.0f), avail - sec_w - id_w - go_w - 3.0f * ui::Dp(6.0f));
        auto line = [&](const char* sec, const std::string& name, const char* id, ImU32 name_col) {
            const ImVec2 a = ImGui::GetCursorScreenPos();
            cdl->AddLine(ImVec2(a.x, a.y), ImVec2(a.x + avail, a.y), ui::kStroke);
            const float ty = a.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f;
            ImGui::PushFont(ui::FontSmall());
            const float sy = a.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f;
            ui::EllipsisText(cdl, ImVec2(a.x, sy), a.x + sec_w, ui::kFaint, sec);
            ImGui::PopFont();
            const bool cut = ui::EllipsisText(cdl, ImVec2(a.x + sec_w + ui::Dp(6.0f), ty), a.x + sec_w + ui::Dp(6.0f) + name_w, name_col, name);
            if (cut && ImGui::IsMouseHoveringRect(ImVec2(a.x + sec_w, a.y), ImVec2(a.x + sec_w + name_w, a.y + row_h)) &&
                ImGui::IsWindowHovered())
                ui::ShowTip(name.c_str());
            ImGui::PushFont(ui::FontMonoSmall());
            ui::EllipsisText(cdl, ImVec2(a.x + sec_w + name_w + ui::Dp(12.0f), a.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f),
                             a.x + sec_w + name_w + ui::Dp(12.0f) + id_w, ui::kFaint, id);
            ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(a.x + avail - go_w, a.y + (row_h - ui::ControlH()) * 0.5f));
            return a;
        };
        int k = 0;
        for (int ai : reaper) {
            const KeyIndexAction& act = g_index.actions[static_cast<size_t>(ai)];
            ImGui::PushID(k++);
            const ImVec2 a = line("REAPER", act.name, act.id.c_str(), ui::kText);
            // D8: Link to... a row (a wheel: the rows that take the wheel).
            if (ui::BeginChipCombo("##linkto", "Link to\xE2\x80\xA6", go_w, false, ImGuiComboFlags_HeightLarge, a.x + avail)) {
                for (const RowRef& r : AllRows()) {
                    if (r.kind == RowRef::Kind::Mouse) continue;
                    if (g_found.input != KbInput::Key && !(r.kind == RowRef::Kind::Nav && kViewNavTable[r.index].wheel_ok)) continue;
                    const std::string item = std::string(LabelOf(r)) + " \xC2\xB7 " + WhereOf(r);
                    if (ui::MenuEntry(item.c_str())) LinkFromFind(r, act.id, act.name);
                }
                ui::EndChipCombo();
            }
            ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + row_h));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            ImGui::PopID();
        }
        for (const RavKeyUse& u : rav) {
            ImGui::PushID(k++);
            RowRef      r;
            std::string sec, name, mode;
            bool        show = false;
            switch (u.kind) {
            case RavKeyUse::Kind::KeyRow:
            case RavKeyUse::Kind::ViewRow:
                r = RowRef{u.kind == RavKeyUse::Kind::KeyRow ? RowRef::Kind::Key : RowRef::Kind::Nav, u.index};
                sec = std::string("RAV \xC2\xB7 ") + WhereOf(r);
                name = LabelOf(r);
                mode = u.shadowed ? "another row's" : RavModeText(r);
                show = true;
                break;
            case RavKeyUse::Kind::EditReserved:
                sec = "RAV \xC2\xB7 Sound Edit";
                name = kEditReservedKeys[u.index].label;
                mode = "fixed";
                break;
            case RavKeyUse::Kind::Mouse:
                r = RowRef{RowRef::Kind::Mouse, u.index};
                sec = std::string("RAV \xC2\xB7 ") + WhereOf(r);
                name = LabelOf(r);
                mode = "fixed";
                show = true;
                break;
            }
            const ImVec2 a = line(sec.c_str(), name, mode.c_str(), ui::kText);
            if (show) {
                ImGui::SetCursorScreenPos(ImVec2(a.x + avail - ui::Dp(60.0f), a.y + (row_h - ui::ControlH()) * 0.5f));
                if (ui::SolidButton("Show", ImVec2(ui::Dp(60.0f), 0.0f))) {
                    g_sel = r;
                    g_sec = SecOf(r);
                    g_search[0] = '\0';
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + row_h));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
    }
    if (rows > 0) ImGui::EndChild();
    const float bottom = ImGui::GetCursorScreenPos().y + ui::Dp(8.0f);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p0.x, bottom), ImVec2(p0.x + w, bottom), ui::kStroke);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, bottom + 1.0f));
}

// ---- the detail pane --------------------------------------------------------------------------

constexpr float kKvKey = 78.0f;

// One fact: its name (muted) and its value, wrapped in the pane.
void Kv(const char* k, const std::string& v, ImU32 col = ui::kText)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(p, ui::kMuted, k);
    ImGui::SetCursorScreenPos(ImVec2(p.x + ui::Dp(kKvKey), p.y));
    ui::ToneText(v, col, /*wrap=*/true, ImGui::GetContentRegionAvail().x);
    ImGui::SetCursorScreenPos(ImVec2(p.x, ImGui::GetCursorScreenPos().y));
}

void WarnBox(const std::string& text)
{
    ui::ConfirmBand band;
    ui::BeginConfirmBand(band, 0.0f, /*warn=*/true);
    ui::ToneText(text, ui::kWarn, /*wrap=*/true);
    ui::EndConfirmBand(band);
}

std::string ActionText(const std::string& id, const char* fallback_name)
{
    const ReaperActionInfo& info = LinkedActionInfo(id);
    std::string             name = !info.name.empty() ? info.name : (fallback_name ? fallback_name : "");
    if (name.empty()) {
        if (info.cmd > 0) return id + " \xC2\xB7 REAPER gives it no name (extension or script not loaded?)";
        if (info.checked) return id + " \xC2\xB7 REAPER does not know it now";
        return id;
    }
    return name + " \xC2\xB7 " + id;
}

void Detail(const RowRef& r)
{
    ui::GroupCaption(WhereOf(r), ui::kMuted);
    ImGui::PushFont(ui::FontSemibold());
    ui::ToneText(LabelOf(r), ui::kText, /*wrap=*/true);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, ui::Dp(2.0f)));

    // Keys.
    const std::vector<Cap> caps = CapsOf(r);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList*  dl = ImGui::GetWindowDrawList();
        dl->AddText(p, ui::kMuted, "Keys");
        if (caps.empty()) {
            dl->AddText(ImVec2(p.x + ui::Dp(kKvKey), p.y), ui::kFaint, NoCapsText(r));
        } else {
            // One line per cap row, wrapped.
            float       x = p.x + ui::Dp(kKvKey), y = p.y - ui::Dp(1.0f);
            const float right = p.x + ImGui::GetContentRegionAvail().x;
            for (const Cap& c : caps) {
                const float cw = ImGui::CalcTextSize(c.text.c_str()).x + ui::Dp(8.0f);
                if (x + cw > right && x > p.x + ui::Dp(kKvKey)) {
                    x = p.x + ui::Dp(kKvKey);
                    y += ImGui::GetTextLineHeight() + ui::Dp(8.0f);
                }
                x += ui::KeyCap(dl, ImVec2(x, y), c.text.c_str(), CapHit(c) ? ui::kWarn : c.col) + ui::Dp(4.0f);
            }
            ImGui::SetCursorScreenPos(ImVec2(p.x, y + ui::Dp(1.0f)));
        }
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() + ui::Dp(4.0f)));
        ImGui::SetCursorScreenPos(ImVec2(p.x, ImGui::GetCursorScreenPos().y));
    }
    std::vector<std::string> warns;
    if (r.kind == RowRef::Kind::Mouse) {
        Kv("Mode", "Fixed");
    } else if (r.kind == RowRef::Kind::Key) {
        const int          id = r.index;
        const ShortcutDef& def = kShortcutTable[id];
        const bool         reaper = ShortcutFollowsReaper(id);
        const std::string  link = ShortcutLink(id);
        Kv("Follows", reaper ? "REAPER" : (link.empty() ? "Own" : "Own \xC2\xB7 linked"));
        if (reaper) Kv("Action", ActionText(ShortcutReaperActionId(id), def.reaper_cmd ? def.reaper_name : def.action_desc));
        if (!link.empty()) Kv("Linked to", ActionText(link, nullptr));
        std::string dflt;
        if (def.reaper_cmd)
            dflt = std::string("REAPER") + (def.reaper_fallback.vk ? " (" + ShortcutBindingLabel(def.reaper_fallback) + " while it has none)" : std::string());
        else
            dflt = def.def.vk ? ShortcutBindingLabel(def.def) : std::string("none");
        Kv("Default", dflt);
        if (id == kShortcutEditSolo || id == kShortcutEditMute) Kv("Saved", "no (listening only)");
        // Conflicts (facts).
        for (const Cap& c : caps)
            if (!c.tip.empty()) warns.push_back(c.tip);
        if (def.context != ShortcutContext::LibraryEdit) {
            std::string library;
            for (const KeyBinding& k : CurrentShortcutKeymap().keys[static_cast<size_t>(id)])
                if (LibraryViewUsesKey(k.vk, k.ctrl, k.shift, k.alt)) library += (library.empty() ? "" : ", ") + ShortcutBindingLabel(k);
            if (!library.empty()) warns.push_back(library + ": the Library view's own while it has the focus");
        }
        if (g_reset_refused == id && CurrentShortcuts()[static_cast<size_t>(id)].vk == 0 && g_reset_blocker >= 0 &&
            ShortcutFollowedAction(id).empty())
            warns.push_back("Cannot get " + ShortcutBindingLabel(def.def) + " back: it is your key for \"" +
                            kShortcutTable[g_reset_blocker].label + "\"");
    } else {
        const ViewNavDef&    def = kViewNavTable[r.index];
        const ViewNavChoice& ch = CurrentViewNavChoices()[static_cast<size_t>(r.index)];
        Kv("Follows", ch.linked_default() ? "REAPER" : (ch.own ? "Own" : "Own \xC2\xB7 linked"));
        if (ch.linked_default()) Kv("Action", std::string(def.reaper_name) + " \xC2\xB7 " + std::to_string(def.reaper_cmd));
        if (!ch.own && !ch.cmd.empty()) Kv("Linked to", ActionText(ch.cmd, nullptr));
        if (!ch.linked_default()) {
            std::string re;
            for (const ViewGesture& g : ReaperViewGestures(CurrentReaperViewKeys(), r.index)) re += (re.empty() ? "" : ", ") + ViewGestureLabel(g);
            Kv("REAPER's", re.empty() ? std::string("none") : re);
        }
        Kv("Takes", def.wheel_ok ? "keys and wheels" : "keys");
        Kv("Default", "REAPER");
    }
    for (const std::string& w : warns) {
        ImGui::Dummy(ImVec2(0.0f, ui::Dp(2.0f)));
        WarnBox(w);
    }
    if (r.kind != RowRef::Kind::Mouse) {
        ImGui::Dummy(ImVec2(0.0f, ui::Dp(6.0f)));
        if (ui::SolidButton("Record key\xE2\x80\xA6")) StartRecord(r);
        ImGui::SameLine();
        ImGui::BeginDisabled(RowOnReaper(r));
        if (ui::SolidButton("Link\xE2\x80\xA6")) OpenLink(TargetOf(r));
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Default")) ResetRow(r);
    }
}

// ---- the record modal ---------------------------------------------------------------------------

// 13-45: a "Timelines" row: a key or the wheel; a gesture another row runs waits for Keep (it is then
// taken from that row).
void NavRecordBody(int row)
{
    const ViewNavDef& def = kViewNavTable[row];
    ImGui::Text(def.wheel_ok ? "Press a key or turn the wheel for %s\xE2\x80\xA6" : "Press a key for %s\xE2\x80\xA6", def.label);
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
        const char* how = !oc ? "" : oc->linked_default() ? " (on REAPER)" : (!oc->own ? " (linked)" : " (your own)");
        ui::ToneText(std::string("Used by ") + (owner >= 0 ? kViewNavTable[owner].label : "another row") + how + ": Keep takes it.",
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
        if (ui::PrimaryButton(("Keep " + label + "##navkeep").c_str())) ViewNavKeepPending();  // ends the recording
    }
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
            ImGui::Text("Press a key for %s\xE2\x80\xA6", kShortcutTable[id].label);
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

// ---- 13-60: the Link modal: a row linked to any REAPER action ------------------------------------
//
// One field: a command ID, words of the action's name, or a shortcut ("ctrl+s", "shift+wheel"); the key
// button takes a pressed key or wheel and keeps the actions REAPER runs on exactly that. The list
// (REAPER's Main section, with each action's keys) narrows as you type; a click puts the action's id in
// the field, a double-click links it. The id is checked with REAPER and the gestures RAV will follow
// shown before Link.

char             g_link_buf[256] = "";
std::vector<int> g_hits;          // the index's actions the field (or the key) matches
std::string      g_hits_for;      // ...for this text
int              g_hits_serial = -1;  // ...in this build of the index
bool             g_hits_key = false;
KbGesture        g_hits_key_g;
bool             g_link_key_on = false;  // the key button holds a gesture
KbGesture        g_link_key;
std::string      g_typed_for;  // the text g_typed was looked up for
ReaperActionInfo g_typed;
bool             g_link_focus = false;
// The picked action's gestures, for this id and this read of reaper-kb.ini.
std::string              g_preview_id;
int                      g_preview_gen = -1;
std::vector<KbGesture>   g_preview;
std::vector<std::string> g_preview_listed;
int                      g_preview_unread = 0;  // listed shortcuts that are no key or wheel (MIDI, OSC...)
bool                     g_preview_known = false;

const char* LinkTargetLabel(int t)
{
    if (t >= 0 && t < kShortcutCount) return kShortcutTable[t].label;
    if (t >= kShortcutCount && t < kShortcutCount + kNavRowCount) return kViewNavTable[t - kShortcutCount].label;
    return "";
}

// The gestures of the action that the target would run on: a KEYS row takes keys only, Zoom in / out
// too; the other view rows keys and wheels.
bool TargetTakesWheel(int t)
{
    return t >= kShortcutCount && kViewNavTable[t - kShortcutCount].wheel_ok;
}

void Refilter()
{
    g_hits.clear();
    g_hits_for = g_link_buf;
    g_hits_serial = g_index_serial;
    g_hits_key = g_link_key_on;
    g_hits_key_g = g_link_key;
    if (g_link_key_on) {
        g_hits = g_index.ActionsOn(g_link_key);
        return;
    }
    KbGesture  typed;
    const bool is_key = ParseTypedShortcut(g_hits_for, &typed, &NormalizeGesture);
    for (int i = 0; i < static_cast<int>(g_index.actions.size()); ++i)
        if (LinkSearchMatch(g_index.actions[static_cast<size_t>(i)], g_hits_for, is_key ? &typed : nullptr)) g_hits.push_back(i);
}

void StartLink()
{
    const int   t = g_link_target;
    std::string cur;
    if (t >= 0 && t < kShortcutCount) cur = ShortcutLink(t);
    else if (t >= kShortcutCount && t < kShortcutCount + kNavRowCount) cur = CurrentViewNavChoices()[static_cast<size_t>(t - kShortcutCount)].cmd;
    std::snprintf(g_link_buf, sizeof(g_link_buf), "%s", cur.c_str());
    EnsureIndex();
    g_typed_for.clear();
    g_typed = ReaperActionInfo{};
    if (LooksLikeCommandId(g_link_buf)) {
        g_typed = LookupReaperAction(g_link_buf);
        g_typed_for = g_link_buf;
    }
    g_link_focus = true;
    g_preview_gen = -1;
    g_link_key_on = false;
    // The list starts whole (the current id in the field is no search).
    g_hits.clear();
    for (int i = 0; i < static_cast<int>(g_index.actions.size()); ++i) g_hits.push_back(i);
    g_hits_for = g_link_buf;
    g_hits_serial = g_index_serial;
    g_hits_key = false;
}

void CloseLink()
{
    g_link_target = -1;
    if (g_capture == Capture::Link) g_capture = Capture::None;
    ImGui::CloseCurrentPopup();
}

// The query read as a shortcut (the list lights the keys it matched).
bool LinkQueryGesture(KbGesture* g)
{
    if (g_link_key_on) {
        *g = g_link_key;
        return true;
    }
    return ParseTypedShortcut(g_link_buf, g, &NormalizeGesture);
}

void LinkModal(bool open_now)
{
    if (open_now) {
        StartLink();
        ImGui::OpenPopup(kLinkId);
    }
    if (!ui::BeginModalCard(kLinkId, 0, ui::Dp(600.0f))) return;
    const int t = g_link_target;
    if (t < 0 || g_close_request) {  // Esc (the viewer's key handling): this modal only
        g_close_request = false;
        CloseLink();
        ui::EndModalCard();
        return;
    }
    const float wrap = ImGui::GetContentRegionAvail().x;
    ui::ToneText(std::string("Link \"") + LinkTargetLabel(t) + "\" to a REAPER action", ui::kText, /*wrap=*/true, wrap);
    ImGui::Dummy(ImVec2(0.0f, ui::Dp(2.0f)));
    if (g_link_focus) {
        ImGui::SetKeyboardFocusHere();
        g_link_focus = false;
    }
    // The field, then the key button.
    const bool        armed = g_capture == Capture::Link;
    const std::string key_label = armed ? std::string("Press a key\xE2\x80\xA6") : (g_link_key_on ? GestureLabel(g_link_key) : std::string("Press keys"));
    const float       key_w = std::max(ui::Dp(130.0f), ImGui::CalcTextSize(key_label.c_str()).x + ui::Dp(42.0f));
    const std::string before = g_link_buf;
    const bool enter = ui::TextField("##linkcmd", "Name, command ID or shortcut (Ctrl+S)", g_link_buf, sizeof(g_link_buf),
                                     wrap - key_w - ui::Dp(6.0f), ImGuiInputTextFlags_EnterReturnsTrue);  // Enter = Link
    if (before != g_link_buf) g_link_key_on = false;  // typing ends a pressed key's filter
    ImGui::SameLine(0.0f, ui::Dp(6.0f));
    if (CaptureButton("##linkkey", key_label.c_str(), armed, g_link_key_on, key_w, true)) {
        if (armed) g_capture = Capture::None;
        else {
            g_capture = Capture::Link;
            g_link_key_on = false;
        }
    }
    if (ImGui::IsItemHovered()) ui::ItemTipText("Press the shortcut to find its action");
    if (g_hits_for != g_link_buf || g_hits_key != g_link_key_on || (g_link_key_on && !(g_hits_key_g == g_link_key)) ||
        g_hits_serial != g_index_serial)  // the index was built again (REAPER's keys read again)
        Refilter();

    // What the field names, when it reads as an id.
    const std::string typed = g_link_buf;
    const bool        is_id = LooksLikeCommandId(typed);
    if (is_id && typed != g_typed_for) {
        g_typed = LookupReaperAction(typed);
        g_typed_for = typed;
    }
    bool ok = false;
    if (!is_id) {
        ImGui::TextUnformatted(" ");
    } else if (g_typed.cmd > 0 && !g_typed.name.empty()) {
        ui::ToneText(std::string("\xE2\x9C\x93 ") + g_typed.name + "  \xC2\xB7  " + g_typed.id, ui::kOk, true, wrap);
        ok = true;
    } else if (g_typed.cmd > 0) {
        ui::ToneText(g_typed.id + ": REAPER gives it no name (extension or script not loaded?)", ui::kWarn, true, wrap);
        ok = true;
    } else if (!g_typed.checked) {
        ui::ToneText(g_typed.id + ": REAPER cannot check it here", ui::kWarn, true, wrap);
        ok = !g_typed.id.empty();
    } else {
        ui::ToneText(std::string("REAPER has no action ") + g_typed.id, ui::kBad, true, wrap);
    }

    // Its gestures (reaper-kb.ini, and the keys REAPER lists for it: what RAV will follow).
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
            std::string& into = (g.input == KbInput::Key || TargetTakesWheel(t)) ? got : skipped;
            into += (into.empty() ? "" : ", ") + GestureLabel(g);
        }
        ui::ToneText(got.empty() ? std::string("No ") + (TargetTakesWheel(t) ? "key or wheel" : "key") + " in REAPER yet"
                                 : "Runs on: " + got,
                     got.empty() ? ui::kWarn : ui::kText, true, wrap);
        if (!skipped.empty()) ui::ToneText("Not used here (keys only): " + skipped, ui::kFaint, true, wrap);
        if (g_preview_known && g_preview_unread > 0) {
            std::string all;
            for (const std::string& d : g_preview_listed) {
                KbGesture g;
                if (!ParseReaperShortcutDesc(d, &g)) all += (all.empty() ? "" : ", ") + d;
            }
            ui::ToneText("Not followed: " + all, ui::kFaint, true, wrap);
        }
        // A KEYS row: a key another RAV row holds stays that one's; a key the Library view uses stays the
        // Library's while it has the focus.
        if (t < kShortcutCount) {
            std::string held, library;
            for (const KbGesture& g : gs) {
                KeyBinding kb;
                if (!KeyBindingOfGesture(g, &kb)) continue;
                const int o = ShortcutKeyOwner(CurrentShortcutKeymap(), kb, t);
                if (o >= 0 && ShortcutFollowedAction(o).empty())
                    held += (held.empty() ? "" : ", ") + ShortcutBindingLabel(kb) + " (\"" + kShortcutTable[o].label + "\")";
                if (LibraryViewUsesKey(kb.vk, kb.ctrl, kb.shift, kb.alt)) library += (library.empty() ? "" : ", ") + ShortcutBindingLabel(kb);
            }
            if (!held.empty()) ui::ToneText("Kept by another RAV row: " + held, ui::kWarn, true, wrap);
            if (!library.empty()) ui::ToneText("The Library view's own while it has the focus: " + library, ui::kWarn, true, wrap);
        }
    }

    // The list of REAPER's actions: name, keys, id.
    ImGui::Dummy(ImVec2(0.0f, ui::Dp(2.0f)));
    const float list_h = ui::Dp(240.0f);
    bool        link_now = false;
    if (g_index.actions.empty()) {
        ui::ToneText("REAPER's action list cannot be read here.", ui::kFaint, true, wrap);
    } else {
        KbGesture  qg;
        const bool q_is_key = LinkQueryGesture(&qg);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(ui::kBg));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(ui::kStroke));
        const bool shown = ImGui::BeginChild("##actionlist", ImVec2(0.0f, list_h), ImGuiChildFlags_Borders);
        ImGui::PopStyleColor(2);
        if (shown) {
            const float row_h = ui::Dp(26.0f);
            if (g_hits.empty()) {
                ui::ToneText(g_link_key_on ? "No REAPER action uses " + GestureLabel(g_link_key) : std::string("No action matches"), ui::kFaint);
            }
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
            ImGuiListClipper clip;
            clip.Begin(static_cast<int>(g_hits.size()), row_h);
            while (clip.Step()) {
                for (int k = clip.DisplayStart; k < clip.DisplayEnd; ++k) {
                    const int hit = g_hits[static_cast<size_t>(k)];
                    if (hit < 0 || hit >= static_cast<int>(g_index.actions.size())) continue;
                    const KeyIndexAction& e = g_index.actions[static_cast<size_t>(hit)];
                    ImGui::PushID(k);
                    const bool sel = is_id && SameCommandId(e.id, g_typed.id);
                    if (ui::SelectRow("##act", sel, ui::kRowHoverStrong, row_h,
                                      ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_NoAutoClosePopups)) {
                        std::snprintf(g_link_buf, sizeof(g_link_buf), "%s", e.id.c_str());
                        g_hits_for = g_link_buf;  // the list stays as it was
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) link_now = true;
                    }
                    const bool   hover = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
                    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                    ImDrawList*  dl = ImGui::GetWindowDrawList();
                    const float  th = ImGui::GetTextLineHeight();
                    const float  id_w = ui::Dp(120.0f), keys_w = ui::Dp(160.0f);
                    const float  id_x = b.x - ui::Dp(6.0f) - id_w;
                    const float  keys_x = id_x - ui::Dp(8.0f) - keys_w;
                    const bool   cut = ui::EllipsisText(dl, ImVec2(a.x + ui::Dp(6.0f), (a.y + b.y - th) * 0.5f), keys_x - ui::Dp(8.0f), ui::kText, e.name);
                    std::vector<Cap> caps;
                    for (const KbGesture& g : e.gestures) {
                        Cap c;
                        c.text = GestureLabel(g);
                        c.col = (q_is_key && g == qg) ? ui::kWarn : ui::kText;
                        caps.push_back(c);
                    }
                    if (caps.empty()) {
                        ImGui::PushFont(ui::FontSmall());
                        dl->AddText(ImVec2(keys_x, (a.y + b.y - ImGui::GetTextLineHeight()) * 0.5f), ui::kFaint, "no shortcut");
                        ImGui::PopFont();
                    } else {
                        DrawCaps(dl, caps, keys_x, keys_x + keys_w, (a.y + b.y - th) * 0.5f - ui::Dp(1.5f), hover);
                    }
                    ImGui::PushFont(ui::FontMonoSmall());
                    ui::EllipsisText(dl, ImVec2(id_x, (a.y + b.y - ImGui::GetTextLineHeight()) * 0.5f), b.x - ui::Dp(6.0f), ui::kFaint, e.id);
                    ImGui::PopFont();
                    if (cut && hover && ImGui::GetIO().MousePos.x < keys_x) ui::ShowTip(e.name.c_str());
                    ImGui::PopID();
                }
            }
            ImGui::PopStyleVar();
        }
        ImGui::EndChild();
    }
    char count[96];
    if (g_link_key_on)
        std::snprintf(count, sizeof(count), "%d of %d actions \xC2\xB7 on %s", static_cast<int>(g_hits.size()),
                      static_cast<int>(g_index.actions.size()), GestureLabel(g_link_key).c_str());
    else
        std::snprintf(count, sizeof(count), "%d of %d actions", static_cast<int>(g_hits.size()), static_cast<int>(g_index.actions.size()));
    if (!g_index.actions.empty()) ui::ToneText(count, ui::kFaint);

    // A double-click on a row names a new id: checked like a typed one.
    if (link_now) {
        g_typed = LookupReaperAction(g_link_buf);
        g_typed_for = g_link_buf;
        ok = g_typed.cmd > 0 || (!g_typed.checked && !g_typed.id.empty());
    }

    ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
    if (ui::SolidButton("Cancel", ImVec2(ui::Dp(90.0f), 0.0f))) {
        CloseLink();
        ui::EndModalCard();
        return;
    }
    // 13-60b: a linked row can be unlinked here: Own, its default key (KEYS) / no gesture yet (VIEW).
    const bool linked_now = t < kShortcutCount ? !ShortcutLink(t).empty()
                                               : !CurrentViewNavChoices()[static_cast<size_t>(t - kShortcutCount)].cmd.empty();
    if (linked_now) {
        ImGui::SameLine();
        if (ui::SolidButton("Unlink", ImVec2(ui::Dp(90.0f), 0.0f))) {
            if (t < kShortcutCount) NoteRefusal(t, ResetShortcutToDefault(t));
            else SetViewNavOwn(t - kShortcutCount);
            CloseLink();
            ui::EndModalCard();
            return;
        }
        if (ImGui::IsItemHovered()) ui::ItemTipText(t < kShortcutCount ? "Back to its default key" : "Back to an own gesture");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!ok);
    if ((ui::PrimaryButton("Link", ImVec2(ui::Dp(90.0f), 0.0f)) || enter || link_now) && ok) {
        if (t < kShortcutCount) LinkShortcutToAction(t, g_typed.id);
        else SetViewNavAction(t - kShortcutCount, g_typed.id);
        g_names.clear();  // its name is looked up again
        CloseLink();
    }
    ImGui::EndDisabled();
    ui::EndModalCard();
}

// ---- the sheet ------------------------------------------------------------------------------------

void TopBar(float w)
{
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float  bar_h = ui::Dp(kTopH);
    const float  y = p0.y + std::floor((bar_h - ui::FieldH()) * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + ui::Dp(12.0f), y));
    ImGui::PushFont(ui::FontSemibold());
    {
        const ImVec2 tp = ImGui::GetCursorScreenPos();
        const float  th = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddText(ImVec2(tp.x, tp.y + std::floor((ui::FieldH() - th) * 0.5f)), ui::kText, "Shortcuts");
        ImGui::Dummy(ImVec2(ImGui::CalcTextSize("Shortcuts").x, ui::FieldH()));
    }
    ImGui::PopFont();
    ImGui::SameLine(0.0f, ui::Dp(14.0f));
    const float right_room = ui::ControlH() + ui::Dp(12.0f);
    const float field_w = std::max(ui::Dp(120.0f), std::min(ui::Dp(240.0f), w * 0.25f));
    ui::TextField("##search", "Search actions", g_search, sizeof(g_search), field_w);
    ImGui::SameLine(0.0f, ui::Dp(8.0f));
    const bool  armed = g_capture == Capture::Find;
    std::string label = armed ? std::string("Press a key or turn the wheel\xE2\x80\xA6")
                              : (g_found_on ? "Find by key: " + GestureLabel(g_found) : std::string("Find by key"));
    const float room = p0.x + w - right_room - ImGui::GetCursorScreenPos().x - ui::Dp(8.0f);
    if (room > ui::Dp(120.0f)) {
        if (ImGui::CalcTextSize(label.c_str()).x + ui::Dp(20.0f) > room) label = armed ? "Press a key\xE2\x80\xA6" : "Find by key";
        if (CaptureButton("##findkey", label.c_str(), armed, g_found_on, std::min(ui::Dp(210.0f), room), false))
            g_capture = armed ? Capture::None : Capture::Find;
        if (ImGui::IsItemHovered() && !armed) ui::ItemTipText("Show what already uses a key or a wheel");
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x + w - right_room + ui::Dp(4.0f), p0.y + std::floor((bar_h - ui::ControlH()) * 0.5f)));
    if (ui::IconOnlyButton("##close", "\xE2\x9C\x95", "Close (Esc)")) ImGui::CloseCurrentPopup();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p0.x, p0.y + bar_h - 1.0f), ImVec2(p0.x + w, p0.y + bar_h - 1.0f), ui::kStroke);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + bar_h));
}

void NavPane(float w, float h)
{
    ui::PaneOpts o;
    o.fill = ui::kSurface;
    o.edge = ui::PaneEdge::None;
    o.pad = 6.0f;
    o.radius = 0.0f;
    if (ui::BeginPane("##secnav", ImVec2(w, h), o)) {
        const std::vector<RowRef>& rows = AllRows();
        for (int s = 0; s < static_cast<int>(Sec::Count); ++s) {
            if (s == static_cast<int>(Sec::Viewer) || s == static_cast<int>(Sec::Mouse)) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + ui::Dp(4.0f), p.y + ui::Dp(3.0f)),
                                                    ImVec2(p.x + ImGui::GetContentRegionAvail().x - ui::Dp(4.0f), p.y + ui::Dp(3.0f)), ui::kStroke);
                ImGui::Dummy(ImVec2(0.0f, ui::Dp(6.0f)));
            }
            int n = 0;
            for (const RowRef& r : rows)
                if (s == static_cast<int>(Sec::All) || static_cast<int>(SecOf(r)) == s) ++n;
            ImGui::PushID(s);
            const bool on = static_cast<int>(g_sec) == s;
            if (ui::SelectRow("##sec", on, ui::kRowHoverStrong, ui::Dp(28.0f))) {
                g_sec = static_cast<Sec>(s);
                // The selection follows into the section.
                if (g_sec != Sec::All && SecOf(g_sel) != g_sec)
                    for (const RowRef& r : rows)
                        if (SecOf(r) == g_sec) {
                            g_sel = r;
                            break;
                        }
            }
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            ImDrawList*  dl = ImGui::GetWindowDrawList();
            const float  th = ImGui::GetTextLineHeight();
            dl->AddText(ImVec2(a.x + ui::Dp(8.0f), (a.y + b.y - th) * 0.5f), on ? ui::kText : ui::kMuted, kSecLabel[s]);
            char cnt[16];
            std::snprintf(cnt, sizeof(cnt), "%d", n);
            ImGui::PushFont(ui::FontSmall());
            const ImVec2 cs = ImGui::CalcTextSize(cnt);
            dl->AddText(ImVec2(b.x - ui::Dp(8.0f) - cs.x, (a.y + b.y - cs.y) * 0.5f), ui::kFaint, cnt);
            ImGui::PopFont();
            ImGui::PopID();
        }
    }
    ui::EndPane();
}

void ListPane(float w, float h)
{
    ui::PaneOpts o;
    o.fill = ui::kSurface;
    o.edge = ui::PaneEdge::None;
    o.pad = 0.0f;
    o.radius = 0.0f;
    o.window_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (ui::BeginPane("##list", ImVec2(w, h), o)) {
        if (g_found_on) FindPanel(w);
        // The header (the rows' columns: their pane less a scrollbar).
        const Cols   c = LayoutCols(w - ImGui::GetStyle().ScrollbarSize);
        const ImVec2 hp = ImGui::GetCursorScreenPos();
        const float  hh = ui::Dp(26.0f);
        ImDrawList*  dl = ImGui::GetWindowDrawList();
        ImFont*      sf = ui::FontSmall();
        const float  ty = hp.y + (hh - sf->FontSize) * 0.5f;
        const char*  heads[] = {"ACTION", "WHERE", "KEYS", "FOLLOWS", "SOURCE"};
        const float  xs[] = {c.label, c.where, c.keys, c.follow, c.src};
        for (int i = 0; i < (c.show_src ? 5 : 4); ++i) ui::AddTextIn(dl, sf, ImVec2(hp.x + xs[i], ty), ui::kMuted, heads[i]);
        dl->AddLine(ImVec2(hp.x, hp.y + hh - 1.0f), ImVec2(hp.x + w, hp.y + hh - 1.0f), ui::kStroke);
        ImGui::SetCursorScreenPos(ImVec2(hp.x, hp.y + hh));
        // The rows.
        const float rows_h = std::max(ui::Dp(40.0f), h - (hp.y + hh - ImGui::GetWindowPos().y));
        if (ImGui::BeginChild("##rows", ImVec2(w, rows_h))) {
            const float rw = ImGui::GetContentRegionAvail().x;
            const Cols& rc = c;
            Sec         last = Sec::Count;
            int         shown = 0;
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
            for (const RowRef& r : AllRows()) {
                if (!RowVisible(r)) continue;
                if (g_sec == Sec::All && SecOf(r) != last) {
                    last = SecOf(r);
                    ImGui::Dummy(ImVec2(0.0f, ui::Dp(10.0f)));
                    ImGui::SetCursorPosX(ui::Dp(10.0f));
                    ui::GroupCaption(kSecLabel[static_cast<int>(last)], ui::kMuted);
                    ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
                }
                DrawRow(r, rc, rw);
                ++shown;
            }
            ImGui::PopStyleVar();
            if (shown == 0) {
                ImGui::Dummy(ImVec2(0.0f, ui::Dp(10.0f)));
                ImGui::SetCursorPosX(ui::Dp(10.0f));
                ui::ToneText("No action matches", ui::kFaint);
            }
            RowMenu();
        }
        ImGui::EndChild();
    }
    ui::EndPane();
}

void SidePane(float w, float h)
{
    ui::PaneOpts o;
    o.fill = ui::kSurface;
    o.edge = ui::PaneEdge::Left;
    o.pad = 12.0f;
    o.radius = 0.0f;
    if (ui::BeginPane("##detail", ImVec2(w, h), o)) Detail(g_sel);
    ui::EndPane();
}

void Footer(float w)
{
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float  fh = ui::Dp(kFootH);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(p0.x, p0.y), ImVec2(p0.x + w, p0.y), ui::kStroke);
    ImGui::PushFont(ui::FontSmall());
    const float ty = p0.y + (fh - ImGui::GetTextLineHeight()) * 0.5f;
    float       x = p0.x + ui::Dp(10.0f);
    const float right = p0.x + w - ui::Dp(10.0f);
    const double age = ViewNavKbAge();
    char         kb[64];
    if (age < 0.0) std::snprintf(kb, sizeof(kb), "reaper-kb.ini not found");
    else if (age < 60.0) std::snprintf(kb, sizeof(kb), "reaper-kb.ini read %d s ago", static_cast<int>(age));
    else if (age < 3600.0) std::snprintf(kb, sizeof(kb), "reaper-kb.ini read %d min ago", static_cast<int>(age / 60.0));
    else std::snprintf(kb, sizeof(kb), "reaper-kb.ini read %d h ago", static_cast<int>(age / 3600.0));
    const float kb_w = ImGui::CalcTextSize(kb).x;
    // D8: the toast of a link, with its Undo: shortened so Undo and the kb.ini age stay inside the sheet.
    if (!g_toast.empty() && ImGui::GetTime() - g_toast_at < 8.0) {
        const float undo_w = g_toast_undo ? ImGui::CalcTextSize("Undo").x + ui::Dp(14.0f) : 0.0f;
        const float max_w = std::max(0.0f, right - x - undo_w - kb_w - ui::Dp(10.0f));
        const std::string text = ui::EllipsizeToWidth(ImGui::GetFont(), std::string("\xE2\x9C\x93 ") + g_toast, max_w);
        if (!text.empty()) {
            dl->AddText(ImVec2(x, ty), ui::kText, text.c_str());
            x += ImGui::CalcTextSize(text.c_str()).x;
        }
        x += ui::Dp(10.0f);
        if (g_toast_undo) {
            ImGui::SetCursorScreenPos(ImVec2(x, p0.y + (fh - ImGui::GetFrameHeight()) * 0.5f));
            if (ui::LinkButton("Undo##toast")) {
                std::function<void()> undo = std::move(g_toast_undo);
                g_toast.clear();
                g_toast_undo = nullptr;
                undo();
            }
            x += ImGui::CalcTextSize("Undo").x + ui::Dp(14.0f);
        }
    } else {
        g_toast.clear();
        g_toast_undo = nullptr;
    }
    if (x + kb_w <= right) dl->AddText(ImVec2(x, ty), ui::kFaint, kb);
    x += kb_w + ui::Dp(14.0f);
    char acts[64];
    std::snprintf(acts, sizeof(acts), "REAPER actions: %d (Main)", static_cast<int>(g_index.actions.size()));
    if (g_index_built && x + ImGui::CalcTextSize(acts).x <= right) dl->AddText(ImVec2(x, ty), ui::kFaint, acts);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + fh));
}

void SheetBody(ImVec2 size)
{
    EnsureIndex();  // again only when REAPER's keys were read again
    g_capture_btn_hot = false;
    const float w = size.x;
    TopBar(w);
    const float  mid_h = std::max(ui::Dp(60.0f), size.y - ui::Dp(kTopH) - ui::Dp(kFootH));
    const bool   side = w >= ui::Dp(900.0f);
    const bool   nav = w >= ui::Dp(600.0f);
    const float  nav_w = nav ? ui::Dp(side ? kNavW : 130.0f) : 0.0f;
    const float  side_w = side ? ui::Dp(kSideW) : 0.0f;
    const ImVec2 mp = ImGui::GetCursorScreenPos();
    if (nav) {
        NavPane(nav_w, mid_h);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(mp.x + nav_w - 1.0f, mp.y), ImVec2(mp.x + nav_w - 1.0f, mp.y + mid_h), ui::kStroke);
        ImGui::SetCursorScreenPos(ImVec2(mp.x + nav_w, mp.y));
    }
    ListPane(w - nav_w - side_w, mid_h);
    if (side) {
        ImGui::SetCursorScreenPos(ImVec2(mp.x + w - side_w, mp.y));
        SidePane(side_w, mid_h);
    }
    ImGui::SetCursorScreenPos(ImVec2(mp.x, mp.y + mid_h));
    Footer(w);

    RecordModal(std::exchange(g_record_now, false));
    LinkModal(std::exchange(g_link_open_now, false));  // 13-60
    // A click anywhere but on the button that armed it (a field, a row) ends a capture: the hook stops
    // taking every key.
    if (g_capture != Capture::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !g_capture_btn_hot) g_capture = Capture::None;
}

// The window opens: names, REAPER's keys and the index read again (an extension loaded since).
void OnOpen()
{
    g_names.clear();
    g_link_target = -1;
    g_capture = Capture::None;
    g_found_on = false;
    g_toast.clear();
    g_toast_undo = nullptr;
    RefreshReaperListedKeys();
    g_index_built = false;
}

}  // namespace

void DrawShortcutsButton(float right_x, float centre_y, ImTextureID icon)
{
    const float btn = ui::Dp(kIconSize + 2.0f * kIconPad);
    const bool  shown = ui::BeginOverlay("##keysbtn", ImVec2(right_x, centre_y - btn * 0.5f), ImVec2(1.0f, 0.0f));
    if (shown) {
        // Frameless like the menu button; lit while the window is open.
        ui::IconOnlyOpts o;
        o.w = btn;
        o.on = g_open;
        o.icon_px = ui::Dp(kIconSize);
        const bool pressed = ui::IconOnlyButton("##keys", icon, nullptr, o);
        if (!icon) {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const ImU32  tint = (g_open || ImGui::IsItemHovered()) ? ui::kText : ui::kMuted;
            DrawKeyboardGlyph(ImGui::GetWindowDrawList(), ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), ui::Dp(kIconSize), tint);
        }
        // A press on the icon while the window was open must not reopen it on the release.
        if (ImGui::IsItemActivated()) g_open_at_press = g_open;
        if (pressed && !g_open_at_press && !g_open) {
            ImGui::OpenPopup(kSheetId);
            OnOpen();
        }
        const ImVec2 bmax = ImGui::GetItemRectMax();

        // D1: a large panel over the viewer, inset, the rest dimmed.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const float          mx = std::min(ui::Dp(22.0f), vp->Size.x * 0.03f);
        const float          bottom = vp->Pos.y + vp->Size.y - std::min(ui::Dp(18.0f), vp->Size.y * 0.03f);
        const float          top = std::min(bmax.y + ui::Dp(8.0f), vp->Pos.y + vp->Size.y * 0.25f);
        const ImVec2         pos(vp->Pos.x + mx, top);
        const ImVec2         size(std::max(ui::Dp(320.0f), vp->Size.x - 2.0f * mx), std::max(ui::Dp(240.0f), bottom - top));
        bool                 backdrop = false;
        const bool           open = ui::BeginSheetModal(kSheetId, pos, size, &backdrop);
        if (open) {
            if (backdrop && ShortcutRecordingId() < 0) {
                g_capture = Capture::None;
                ImGui::CloseCurrentPopup();
            }
            SheetBody(size);
            // Esc that nothing over it took (the Link modal, the row menu): a Find result clears first,
            // then the window closes. Not while recording (Esc cancels it) nor with a menu or combo open.
            if (g_close_request && ShortcutRecordingId() < 0 && g_link_target < 0) {
                if (g_found_on) g_found_on = false;
                else ImGui::CloseCurrentPopup();
            }
            ui::EndSheetModal();
        }
        g_open = open;
    } else {
        g_open = false;
    }
    g_close_request = false;
    ui::EndOverlay();

    // The window went away (e.g. the viewer closed it): no recording or capture without it.
    if (!g_open) {
        if (ShortcutRecordingId() >= 0) CancelShortcutRecording();
        g_capture = Capture::None;
    }
}

bool ShortcutsPopupOpen()
{
    return g_open;
}

void RequestCloseShortcutsPopup()
{
    g_close_request = true;
}

bool ShortcutsKeyCaptureArmed()
{
    return g_open && g_capture != Capture::None;
}

namespace {

void Captured(const KbGesture& g, bool ctrl, bool shift, bool alt)
{
    if (g_capture == Capture::Find) {
        g_found = g;
        g_found_on = true;
        g_found_char_row = g.input == KbInput::Key ? ViewNavRowForKeyDown(static_cast<unsigned>(g.code), ctrl, shift, alt) : -1;
        EnsureIndex();
    } else if (g_capture == Capture::Link) {
        g_link_key = g;
        g_link_key_on = true;
        g_link_buf[0] = '\0';
    }
    g_capture = Capture::None;
}

int ModsOf(bool ctrl, bool shift, bool alt)
{
    return (ctrl ? kViewModCtrl : 0) | (shift ? kViewModShift : 0) | (alt ? kViewModAlt : 0);
}

}  // namespace

void ShortcutsCaptureKey(unsigned vk_code, bool ctrl, bool shift, bool alt)
{
    if (g_capture == Capture::None) return;
    if (vk_code == vk::kEscape) {
        g_capture = Capture::None;
        return;
    }
    if (!IsBindableKey(vk_code)) return;  // a modifier alone: still waiting
    KbGesture g;
    g.input = KbInput::Key;
    g.code = static_cast<int>(vk_code);
    g.mods = ModsOf(ctrl, shift, alt);
    Captured(g, ctrl, shift, alt);
}

void ShortcutsCaptureWheel(bool horizontal, bool ctrl, bool shift, bool alt)
{
    if (g_capture == Capture::None) return;
    KbGesture g;
    g.input = horizontal ? KbInput::HWheel : KbInput::Wheel;
    g.mods = ModsOf(ctrl, shift, alt);
    Captured(g, ctrl, shift, alt);
}

void CancelShortcutsKeyCapture()
{
    g_capture = Capture::None;
}

}  // namespace rav

#endif  // _WIN32
