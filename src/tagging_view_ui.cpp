// SPDX-License-Identifier: MIT
//
// See tagging_view_ui.h.

#include "tagging_view_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <imgui.h>

#include "bone_roles.h"
#include "event_list.h"
#include "file_dialogs.h"
#include "item_rules.h"
#include "preset_menu_model.h"
#include "preset_store.h"
#include "reaper_api.h"
#include "rule_record.h"
#include "shortcuts.h"  // LoadPrefFloat / SavePrefFloat
#include "skeleton_view.h"  // spec 10-3c: the bone selected in the 3D view ("+")
#include "strip_view.h"
#include "tag_markers.h"
#include "tagging_session.h"
#include "tagging_signal.h"
#include "ui_theme.h"
#include "video_view.h"  // TaggingViewActive, QueueVideoSeek (the playhead move runs outside the frame)

namespace rav {
namespace {

// Strings outside ASCII are spelled as UTF-8 escapes (the build has no /utf-8).
#define RAV_DOT "\xC2\xB7"  // middle dot

constexpr char kPrefStripHeight[] = "tagging.strip_height";
constexpr char kPrefPanelWidth[] = "tagging.panel_width";
constexpr float kStripDefaultHeight = 230.0f;
constexpr float kPanelMinW = 280.0f;
constexpr float kPanelMaxW = 600.0f;
constexpr float kPanelDefaultW = 330.0f;

// The mock-up's "bad" red (missing roles): its stroke and its text.
constexpr ImU32 kBad = IM_COL32(0xF0, 0x6C, 0x6C, 0xFF);
constexpr ImU32 kBadText = IM_COL32(0xFF, 0xB3, 0xB3, 0xFF);
constexpr ImU32 kPlayhead = IM_COL32(0xDF, 0xE3, 0xEA, 0xFF);

float g_strip_h = -1.0f;  // -1 = not read yet
float g_panel_w = -1.0f;
StripGripState g_strip_grip;
StripViewState g_view;

// The panel's left-edge grip.
bool  g_panel_drag = false;
float g_panel_press_x = 0.0f;
float g_panel_start_w = 0.0f;

int        g_sel = 0;             // the selected rule
MediaItem* g_sel_item = nullptr;  // the item g_sel belongs to

// A drag in the strip's lanes: a threshold or re-arm line, a scrub, or (story 10-4) an event.
enum class StripDrag { None, Scrub, Threshold, Margin, Event };
StripDrag g_drag = StripDrag::None;
int       g_drag_block = -1;
int       g_drag_cond = -1;
bool      g_drag_moved = false;

// Story 10-4 -- the selected event: its rule, its time and which kind of event it is (a
// detection, shown or suppressed; a user event; an orphan suppression). Found again each
// frame in the model's event list (a detection a threshold change moved a little is followed).
enum class EvGroup { Detection, User, Orphan };
struct SelEvent {
    bool    on = false;
    int     block = -1;
    double  t = 0.0;
    EvGroup group = EvGroup::Detection;
};
SelEvent g_sel_ev;
// The event being dragged: as it was at the press, and where it is now (clip time).
ShownEvent g_drag_ev;
double     g_drag_ev_entry_t = 0.0;  // its record entry's time (a user event)
double     g_drag_ev_to = 0.0;
float      g_drag_press_x = 0.0f;
bool       g_menu_request = false;  // the footer's "Change"

// The marker name being typed (kept while its field is active).
char       g_name_buf[256] = {};
int        g_name_block = -1;        // the rule the buffer belongs to
MediaItem* g_name_item = nullptr;    // ...on this item
bool       g_name_active = false;    // the field is being typed into

// The preset list, re-read at most once a second (and when the menu opens).
std::vector<PresetInfo> g_presets;
double                  g_presets_at = -1.0;

const std::vector<PresetInfo>& CachedPresets(bool refresh = false)
{
    const double now = ImGui::GetTime();
    if (refresh || g_presets_at < 0.0 || now - g_presets_at >= 1.0) {
        g_presets = ListPresets(RulesResourceRoot());
        g_presets_at = now;
    }
    return g_presets;
}

float PanelWidth()
{
    if (g_panel_w < 0.0f) g_panel_w = LoadPrefFloat(kPrefPanelWidth, kPanelDefaultW);
    return std::min(std::max(g_panel_w, kPanelMinW), kPanelMaxW);
}

float StripMaxHeight(float client_h)
{
    return std::max(kTaggingStripMinHeight, std::floor(client_h * 0.6f));
}

void FormatTime(double t, char* out, size_t cap)
{
    if (!std::isfinite(t) || t < 0.0) t = 0.0;
    const long long ms = std::llround(t * 1000.0);
    const int minutes = static_cast<int>(ms / 60000);
    const double seconds = static_cast<double>(ms % 60000) / 1000.0;
    std::snprintf(out, cap, "%d:%06.3f", minutes, seconds);
}

double Playhead(bool* playing)
{
    const bool p = (GetPlayStateEx(nullptr) & 1) != 0;
    if (playing) *playing = p;
    return p ? GetPlayPosition2Ex(nullptr) : GetCursorPositionEx(nullptr);
}

double ProjectFps()
{
    bool drop = false;
    const double fps = TimeMap_curFrameRate(nullptr, &drop);
    return (std::isfinite(fps) && fps > 0.0) ? fps : 0.0;
}

// ---- Colours ---------------------------------------------------------------------------------

ImU32 FromRecordColor(uint32_t c)
{
    if (!(c & 0x1000000u)) return ui::kMuted;  // none: neutral
    return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 0xFF);
}

uint32_t ToRecordColor(ImU32 c)
{
    const uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    return 0x1000000u | (r << 16) | (g << 8) | b;
}

ImU32 WithAlpha(ImU32 c, int a)
{
    return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a & 0xFF) << IM_COL32_A_SHIFT);
}

ImU32 RuleColor(const Block& b)
{
    return b.enabled ? FromRecordColor(b.color) : ui::kFaint;
}

// ---- Small drawn icons (the default font has no symbols) -------------------------------------

void IconCross(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
    dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, 1.4f);
    dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), col, 1.4f);
}

void IconCheck(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
    const ImVec2 pts[3] = {ImVec2(c.x - r, c.y), ImVec2(c.x - r * 0.3f, c.y + r * 0.7f), ImVec2(c.x + r, c.y - r * 0.7f)};
    dl->AddPolyline(pts, 3, col, ImDrawFlags_None, 1.6f);
}

void IconDuplicate(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
    dl->AddRect(ImVec2(c.x - r, c.y - r * 0.4f), ImVec2(c.x + r * 0.4f, c.y + r), col, 1.5f);
    dl->AddRect(ImVec2(c.x - r * 0.4f, c.y - r), ImVec2(c.x + r, c.y + r * 0.4f), col, 1.5f);
}

void IconLock(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool closed)
{
    dl->AddRectFilled(ImVec2(c.x - r, c.y - r * 0.1f), ImVec2(c.x + r, c.y + r), col, 1.5f);
    const ImVec2 sc(c.x, c.y - r * 0.1f);
    const float  sr = r * 0.62f;
    if (closed) {
        dl->PathArcTo(sc, sr, 3.14159265f, 2.0f * 3.14159265f, 10);
    } else {
        dl->PathArcTo(ImVec2(sc.x - r * 0.0f, sc.y - r * 0.25f), sr, 3.14159265f, 1.75f * 3.14159265f, 10);
    }
    dl->PathStroke(col, ImDrawFlags_None, 1.5f);
}

void IconPlus(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
    dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col, 1.6f);
    dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), col, 1.6f);
}

void IconGear(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
    for (int i = 0; i < 8; ++i) {
        const float a = static_cast<float>(i) * 3.14159265f / 4.0f;
        dl->AddLine(ImVec2(c.x + std::cos(a) * r * 0.55f, c.y + std::sin(a) * r * 0.55f),
                    ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r), col, 2.0f);
    }
    dl->AddCircle(c, r * 0.6f, col, 16, 1.6f);
}

// A frameless square icon button; `draw` paints the icon at its centre.
bool IconButton(const char* id, float size, const std::function<void(ImDrawList*, ImVec2, ImU32)>& draw,
                bool enabled = true)
{
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (!enabled) ImGui::BeginDisabled();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = enabled && ImGui::IsItemHovered();
    if (!enabled) ImGui::EndDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered) dl->AddRectFilled(p0, ImVec2(p0.x + size, p0.y + size), ui::kHover, ui::kRadiusSm);
    draw(dl, ImVec2(p0.x + size * 0.5f, p0.y + size * 0.5f), enabled ? (hovered ? ui::kText : ui::kMuted) : ui::kFaint);
    return pressed && enabled;
}

// The rule's on/off switch (the mock-up's .sw). Returns true when clicked.
bool Switch(const char* id, bool on)
{
    const float w = 26.0f, h = 16.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float y = p0.y + std::floor((ImGui::GetFrameHeight() - h) * 0.5f);
    ImGui::InvisibleButton(id, ImVec2(w, ImGui::GetFrameHeight()));
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p0.x, y), ImVec2(p0.x + w, y + h), on ? ui::kAccent : ui::kStrokeStrong, h * 0.5f);
    dl->AddCircleFilled(ImVec2(on ? p0.x + w - h * 0.5f : p0.x + h * 0.5f, y + h * 0.5f), h * 0.5f - 2.0f,
                        IM_COL32(0xC9, 0xCE, 0xD8, 0xFF), 12);
    return clicked;
}

// ---- Edits -----------------------------------------------------------------------------------

Block* BlockAt(ItemRules& r, int b)
{
    return (b >= 0 && b < static_cast<int>(r.blocks.size())) ? &r.blocks[static_cast<size_t>(b)] : nullptr;
}

Condition* CondAt(ItemRules& r, int b, int c)
{
    Block* blk = BlockAt(r, b);
    return (blk && c >= 0 && c < static_cast<int>(blk->conditions.size())) ? &blk->conditions[static_cast<size_t>(c)]
                                                                          : nullptr;
}

std::string RuleTitle(const Block& b)
{
    return b.marker.empty() ? std::string("(unnamed rule)") : b.marker;
}

// Writes run after the widgets are drawn (end of the strip / the panel): an edit replaces the
// rules the widgets still read this frame. Each one is still one undo point, at once.
std::vector<std::function<void()>> g_after;

void Later(std::function<void()> f)
{
    g_after.push_back(std::move(f));
}

void RunLater()
{
    std::vector<std::function<void()>> run;
    run.swap(g_after);
    for (const std::function<void()>& f : run) f();
}

// One edit of the rules, one undo point named `desc`. `fn` returns false to change nothing.
// It is written on the item it was made on (`item`, default: the current one), never on the
// next item the playhead reached meanwhile.
void Edit(const std::string& desc, std::function<bool(ItemRules&)> fn, MediaItem* item = nullptr)
{
    if (!item) item = GetTaggingModel().item;
    // The footer's last Commit / Cancel line goes stale with the write (ModifyItemRules' hook,
    // which also tells a preview-marker failure there).
    Later([desc, fn, item]() { TaggingEdit(desc.c_str(), fn, item); });
}

// Story 10-4 follow-up: a manual event correction (move, add, suppress / restore / delete,
// typed time). 10-4 fb-4: written as every other gesture (the record + the item's preview
// markers, one undo point); the committed markers wait for Commit.
void EventEdit(const std::string& desc, std::function<bool(ItemRules&)> fn, MediaItem* item = nullptr)
{
    if (!item) item = GetTaggingModel().item;
    Later([desc, fn, item]() { TaggingEditEvent(desc.c_str(), fn, item); });
}

// ---- Story 10-4: the event list ------------------------------------------------------------------

EvGroup GroupOf(ShownKind k)
{
    return k == ShownKind::User ? EvGroup::User : k == ShownKind::Orphan ? EvGroup::Orphan : EvGroup::Detection;
}

void SelectEvent(const ShownEvent& e)
{
    g_sel_ev.on = true;
    g_sel_ev.block = e.block;
    g_sel_ev.t = e.t;
    g_sel_ev.group = GroupOf(e.kind);
    g_sel = e.block;
}

void SelectEventAt(int block, double t, EvGroup group)
{
    g_sel_ev.on = true;
    g_sel_ev.block = block;
    g_sel_ev.t = t;
    g_sel_ev.group = group;
    g_sel = block;
}

void ClearEventSelection()
{
    g_sel_ev.on = false;
}

// The selected event in the model's list (null when it is gone; the selection then clears).
const ShownEvent* SelectedEvent(const TaggingModel& m)
{
    if (!g_sel_ev.on) return nullptr;
    const ShownEvent* best = nullptr;
    double best_d = 1e300;
    for (const ShownEvent& e : m.events) {
        if (e.block != g_sel_ev.block || GroupOf(e.kind) != g_sel_ev.group) continue;
        const double d = std::fabs(e.t - g_sel_ev.t);
        // A user event or an orphan stays where it is; a detection may move a little.
        const double tol = g_sel_ev.group == EvGroup::Detection ? 0.06 : 1e-6;
        if (d <= tol && d < best_d) {
            best = &e;
            best_d = d;
        }
    }
    if (best) g_sel_ev.t = best->t;
    else if (m.detected && g_after.empty()) g_sel_ev.on = false;  // gone (not an edit still queued)
    return best;
}

// The time of the record entry behind a shown event (a suppression's own time for a
// suppressed detection), in the rules shown; NAN when there is none.
double EntryTime(const ShownEvent& e)
{
    const ItemRules& r = TaggingShownRules();
    if (e.entry < 0 || e.entry >= static_cast<int>(r.events.size())) return std::nan("");
    return r.events[static_cast<size_t>(e.entry)].t;
}

// The entry of `kind` on rule `block` at exactly time t, or -1.
int FindEntry(const ItemRules& r, EventKind kind, int block, double t)
{
    for (size_t i = 0; i < r.events.size(); ++i) {
        const EventEntry& x = r.events[i];
        if (x.kind == kind && x.block == block && x.t == t) return static_cast<int>(i);
    }
    return -1;
}

std::string RuleNameAt(const ItemRules& r, int b)
{
    return (b >= 0 && b < static_cast<int>(r.blocks.size())) ? RuleTitle(r.blocks[static_cast<size_t>(b)])
                                                              : std::string("rule");
}

// A user event on rule `block` at clip time t: its values measured there.
void AddUserEvent(int block, double t)
{
    const ItemRules& shown = TaggingShownRules();
    if (block < 0 || block >= static_cast<int>(shown.blocks.size()) || !std::isfinite(t)) return;
    EventEdit("RAV: Add event (" + RuleNameAt(shown, block) + ")", [block, t](ItemRules& r) {
        if (!BlockAt(r, block)) return false;
        double s = 0.0, v = 0.0;
        TaggingMeasureEvent(r, block, t, &s, &v);
        r.events.push_back(MakeUserEvent(block, t, s, v));
        return true;
    });
    SelectEventAt(block, t, EvGroup::User);
}

// Right-click / Del: a detection is suppressed, a suppression restored, a user event deleted.
void ToggleEvent(const ShownEvent& e)
{
    const ItemRules& shown = TaggingShownRules();
    const std::string rule = RuleNameAt(shown, e.block);
    const int block = e.block;
    if (e.kind == ShownKind::Detected) {
        const double t = e.t;
        EventEdit("RAV: Suppress event (" + rule + ")", [block, t](ItemRules& r) {
            if (!BlockAt(r, block)) return false;
            r.events.push_back(MakeSuppression(block, t));
            return true;
        });
    } else if (e.kind == ShownKind::Suppressed || e.kind == ShownKind::Orphan) {
        const double t0 = EntryTime(e);
        if (!std::isfinite(t0)) return;
        EventEdit("RAV: Restore event (" + rule + ")", [block, t0](ItemRules& r) {
            const int i = FindEntry(r, EventKind::Suppress, block, t0);
            if (i < 0) return false;
            r.events.erase(r.events.begin() + i);
            return true;
        });
    } else if (e.kind == ShownKind::User) {
        const double t0 = EntryTime(e);
        if (!std::isfinite(t0)) return;
        EventEdit("RAV: Delete event (" + rule + ")", [block, t0](ItemRules& r) {
            const int i = FindEntry(r, EventKind::User, block, t0);
            if (i < 0) return false;
            r.events.erase(r.events.begin() + i);
            return true;
        });
        ClearEventSelection();
    }
}

// A drag of a detection makes it the user's own at `to` (the detection is suppressed); a user
// event moves there. Its values are measured at its new time. One undo point.
void MoveEvent(const ShownEvent& e, double entry_t, double to)
{
    const ItemRules& shown = TaggingShownRules();
    const std::string rule = RuleNameAt(shown, e.block);
    const int block = e.block;
    if (e.kind == ShownKind::Detected) {
        const double det_t = e.t;
        EventEdit("RAV: Move event (" + rule + ")", [block, det_t, to](ItemRules& r) {
            if (!BlockAt(r, block)) return false;
            double s = 0.0, v = 0.0;
            TaggingMeasureEvent(r, block, to, &s, &v);
            r.events.push_back(MakeSuppression(block, det_t));
            r.events.push_back(MakeUserEvent(block, to, s, v));
            return true;
        });
    } else if (e.kind == ShownKind::User) {
        if (!std::isfinite(entry_t)) return;
        EventEdit("RAV: Move event (" + rule + ")", [block, entry_t, to](ItemRules& r) {
            const int i = FindEntry(r, EventKind::User, block, entry_t);
            if (i < 0) return false;
            EventEntry& x = r.events[static_cast<size_t>(i)];
            x.t = to;
            TaggingMeasureEvent(r, block, to, &x.strength, &x.speed);
            x.has_strength = x.has_speed = true;
            return true;
        });
    } else {
        return;
    }
    SelectEventAt(block, to, EvGroup::User);
}

// Clip time for project time p on the item's first pass, held inside it (a drop past its end
// lands on its last instant).
bool ClipTimeForDrop(const ItemClipMap& map, double p, double* clip_t)
{
    ClipPass pass;
    if (!FirstClipPass(map, &pass)) return false;
    p = std::min(std::max(p, pass.p0), pass.p1);
    return FirstPassClipTime(map, p, clip_t);
}

// Whether events can be edited now (rules shown and detection ran).
bool EventsEditable(const TaggingModel& m)
{
    return m.item && m.has_record && m.detected && m.missing_count == 0;
}

// The marker kinds a shown event draws, in a notify row from ry to ry + rh at x.
void DrawEventMark(ImDrawList* dl, const ShownEvent& e, float x, float ry, float rh, ImU32 col, bool selected)
{
    const float y0 = ry + 2.0f, y1 = ry + rh - 2.0f;
    if (e.kind == ShownKind::Orphan) {
        for (float y = y0; y < y1; y += 4.0f)
            dl->AddLine(ImVec2(x, y), ImVec2(x, std::min(y + 2.0f, y1)), WithAlpha(col, 0x73), 1.0f);
    } else {
        const ImU32 c = e.kind == ShownKind::Suppressed ? WithAlpha(col, 0x59) : col;
        dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), c, selected ? 1.5f : 1.0f);
        const ImVec2 a(x - 4.5f, y0), b(x + 4.5f, y0), d(x, y0 + 6.0f);
        if (e.kind == ShownKind::User) dl->AddTriangle(a, b, d, c, 1.5f);
        else dl->AddTriangleFilled(a, b, d, c);
    }
    if (e.kind == ShownKind::Suppressed || e.kind == ShownKind::Orphan)
        dl->AddLine(ImVec2(x - 4.0f, y1 - 2.0f), ImVec2(x + 5.0f, y0 + 4.0f), ui::kText, 1.3f);
    if (e.kind == ShownKind::User) {
        const float fs = ImGui::GetFontSize() * 0.85f;
        dl->AddText(ImGui::GetFont(), fs, ImVec2(x + 4.0f, y1 - fs), col, "you");
    }
    if (selected) dl->AddRect(ImVec2(x - 7.0f, ry + 1.0f), ImVec2(x + 8.0f, ry + rh - 1.0f), kPlayhead, 3.0f);
}

// A number field bound to one value of the rules (display units): drag previews, release /
// Enter writes one undo point, Esc cancels.
void NumberField(const char* id, double display_value, double step, int decimals, const char* unit, float width,
                 const std::string& undo, const std::function<bool(ItemRules&, double)>& set)
{
    double v = display_value;
    switch (ui::DragNumber(id, &v, step, decimals, unit, width)) {
    case ui::DragNumberEvent::Live:
        Later([set, v, item = GetTaggingModel().item]() {
            if (GetTaggingModel().item != item) return;
            ItemRules p = TaggingShownRules();
            if (set(p, v)) TaggingPreview(p);
        });
        break;
    case ui::DragNumberEvent::Commit:
        Edit(undo, [set, v](ItemRules& r) { return set(r, v); });
        break;
    case ui::DragNumberEvent::Cancel:
        Later([]() { TaggingCancelPreview(); });
        break;
    default:
        break;
    }
}

// The next unused palette colour for a new rule.
ImU32 NextRuleColor(const std::vector<Block>& blocks)
{
    for (int i = 0; i < 8; ++i) {
        const uint32_t c = ToRecordColor(ui::kCurvePalette[(7 + i) % 8]);
        bool used = false;
        for (const Block& b : blocks)
            if (b.color == c) used = true;
        if (!used) return ui::kCurvePalette[(7 + i) % 8];
    }
    return ui::kCurvePalette[blocks.size() % 8];
}

// ---- Bone menus ------------------------------------------------------------------------------

std::string RefLabel(const std::vector<int>& ids)
{
    std::string s;
    for (size_t i = 0; i < ids.size(); ++i) s += (i ? "+" : "") + BoneRefLabel(ids[i]);
    return s.empty() ? std::string("?") : s;
}

// 10-4 follow-up: a bone of this skeleton with a parent and a child (a joint angle can be
// read there).
bool IsJointBone(const TaggingModel& m, int bone)
{
    if (bone < 0 || bone >= static_cast<int>(m.bone_parents.size())) return false;
    if (m.bone_parents[static_cast<size_t>(bone)] < 0) return false;
    for (int p : m.bone_parents)
        if (p == bone) return true;
    return false;
}

// A bone reference's bone on the loaded skeleton: a role through its mapping, a raw bone by
// its name. -1 when it does not resolve (unmapped, unknown).
int ResolveOnSkeleton(const TaggingModel& m, int id)
{
    if (id >= 0 && id < static_cast<int>(Role::Count)) {
        const int b = id < static_cast<int>(m.role_to_bone.size()) ? m.role_to_bone[static_cast<size_t>(id)] : -1;
        return (b >= 0 && b < static_cast<int>(m.bone_names.size())) ? b : -1;
    }
    // Story 10-3e: a custom role (or a colleague's) through its mapping by key.
    const std::string role_key = RoleKeyOfRef(id);
    if (!role_key.empty()) {
        const auto it = m.custom_role_to_bone.find(role_key);
        const int  b = it != m.custom_role_to_bone.end() ? it->second : -1;
        return (b >= 0 && b < static_cast<int>(m.bone_names.size())) ? b : -1;
    }
    for (size_t i = 0; i < m.bone_names.size(); ++i)
        if (BoneRefForBone(m.bone_names[i]) == id) return static_cast<int>(i);
    return -1;
}

// A role read by the rules but in no list ("tail end · not in your roles").
constexpr char kNotInRoles[] = " \xC2\xB7 not in your roles";

// Story 10-3e: the non-built-in role keys the Bone menu and the roles window list: the
// user's roles (in their order), then the keys the item's rules read that are in no list.
std::vector<std::string> CustomRoleRowKeys(const TaggingModel& m, std::vector<char>* foreign = nullptr)
{
    std::vector<std::string> keys;
    for (const CustomRole& c : m.custom_roles) keys.push_back(c.key);
    if (foreign) foreign->assign(keys.size(), 0);
    for (const std::string& k : m.rule_role_keys)
        if (std::find(keys.begin(), keys.end(), k) == keys.end()) {
            keys.push_back(k);
            if (foreign) foreign->push_back(1);
        }
    return keys;
}

// The Bone menu's list: the roles, then the raw bones. Returns the picked id, or -1.
// joints_only (a joint angle): a role or bone without a parent or a child is greyed out
// (a role is checked through its bone on this skeleton; an unmapped role stays offered).
int BoneMenuItems(int current_single, bool joints_only = false)
{
    int picked = -1;
    const TaggingModel& m = GetTaggingModel();
    const bool know_skeleton = joints_only && !m.bone_parents.empty();
    ImGui::TextDisabled("Roles");
    for (int r = 0; r < static_cast<int>(Role::Count); ++r) {
        const bool sel = current_single == r;
        const int bone = r < static_cast<int>(m.role_to_bone.size()) ? m.role_to_bone[static_cast<size_t>(r)] : -1;
        const bool off = know_skeleton && bone >= 0 && !IsJointBone(m, bone);
        if (ImGui::Selectable(RoleShortLabel(static_cast<Role>(r)).c_str(), sel,
                              off ? ImGuiSelectableFlags_Disabled : ImGuiSelectableFlags_None))
            picked = r;
    }
    // Story 10-3e: the user's roles, then a colleague's the item reads, with the same greying.
    {
        std::vector<char>              foreign;
        const std::vector<std::string> keys = CustomRoleRowKeys(m, &foreign);
        if (!keys.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("Your roles");
        }
        for (size_t k = 0; k < keys.size(); ++k) {
            const int  id = BoneRefId(std::string("role:") + keys[k]);
            const auto it = m.custom_role_to_bone.find(keys[k]);
            const int  bone = it != m.custom_role_to_bone.end() ? it->second : -1;
            const bool off = know_skeleton && bone >= 0 && !IsJointBone(m, bone);
            ImGui::PushID(keys[k].c_str());
            const std::string label = BoneRefLabel(id) + (foreign[k] ? kNotInRoles : "");
            if (ImGui::Selectable(label.c_str(), current_single == id,
                                  off ? ImGuiSelectableFlags_Disabled : ImGuiSelectableFlags_None))
                picked = id;
            if (foreign[k] && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("Not in your roles (from these rules)");
            ImGui::PopID();
        }
    }
    if (!m.bone_names.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Bones");
        for (size_t i = 0; i < m.bone_names.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const int id = BoneRefForBone(m.bone_names[i]);
            const bool off = know_skeleton && !IsJointBone(m, static_cast<int>(i));
            if (ImGui::Selectable(m.bone_names[i].c_str(), current_single == id,
                                  off ? ImGuiSelectableFlags_Disabled : ImGuiSelectableFlags_None))
                picked = id;
            ImGui::PopID();
        }
    }
    return picked;
}

// The width a chip takes for this preview text.
float ChipWidth(const char* preview, float min_w = 40.0f)
{
    return std::max(min_w, ImGui::CalcTextSize(preview).x + 30.0f);
}

// 10-3 fb-2: the right edge the next chip stays within (set by SentenceChip; 0 = the window's).
float g_chip_right = 0.0f;

// A small combo ("chip", the mock-up's select.chip) sized to its preview text, a small
// chevron on its right.
bool BeginChip(const char* id, const char* preview, float min_w = 40.0f, bool disabled = false)
{
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;
    if (g_chip_right > 0.0f) avail = std::min(avail, g_chip_right - p0.x);
    g_chip_right = 0.0f;
    const float w = std::min(ChipWidth(preview, min_w), std::max(min_w, avail));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(w);
    const bool open = ImGui::BeginCombo(id, preview, ImGuiComboFlags_HeightLarge | ImGuiComboFlags_NoArrowButton);
    const ImVec2 c(p0.x + w - 10.0f, p0.y + h * 0.5f);
    dl->AddTriangleFilled(ImVec2(c.x - 3.5f, c.y - 1.5f), ImVec2(c.x + 3.5f, c.y - 1.5f), ImVec2(c.x, c.y + 2.5f),
                          disabled ? ui::kFaint : ui::kMuted);
    return open;
}

// ---- 10-3 fb-2: compartments (strip tracks, panel cards) and aligned sentences --------------

// Text from p, shortened with "..." (whole glyphs) so it ends before max_x. True when shortened.
bool EllipsisText(ImDrawList* dl, ImVec2 p, float max_x, ImU32 col, const std::string& text)
{
    const float avail = max_x - p.x;
    if (ImGui::CalcTextSize(text.c_str()).x <= avail) {
        dl->AddText(p, col, text.c_str());
        return false;
    }
    const float dots = ImGui::CalcTextSize("...").x;
    size_t n = text.size();
    while (n > 0) {
        do {
            --n;
        } while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80);  // a whole UTF-8 glyph
        if (ImGui::CalcTextSize(text.c_str(), text.c_str() + n).x + dots <= avail) break;
    }
    std::string s = text.substr(0, n);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    s += "...";
    dl->PushClipRect(ImVec2(p.x, p.y - 1.0f), ImVec2(std::max(p.x, max_x), p.y + ImGui::GetFontSize() + 1.0f), true);
    dl->AddText(p, col, s.c_str());
    dl->PopClipRect();
    return true;
}

// A strip track: its header cell (the gutter, x0..lx) and its lane (lx..x1), filled.
void TrackFill(ImDrawList* dl, float x0, float lx, float x1, float y0, float y1, ImU32 lane_fill)
{
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(lx, y1), ui::kRaised, ui::kRadiusSm, ImDrawFlags_RoundCornersLeft);
    dl->AddRectFilled(ImVec2(lx, y0), ImVec2(x1, y1), lane_fill, ui::kRadiusSm, ImDrawFlags_RoundCornersRight);
}

// A 3 px stripe on the left of a rounded box (p0..p1): the box's own rounded rect clipped to
// the stripe, so it follows the box's corners.
void RuleStripe(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col, float radius)
{
    dl->PushClipRect(p0, ImVec2(p0.x + 3.0f, p1.y), true);
    dl->AddRectFilled(p0, p1, col, radius);
    dl->PopClipRect();
}

// A strip track's edges: the divider between header and lane, one edge around both.
void TrackEdge(ImDrawList* dl, float x0, float lx, float x1, float y0, float y1, ImU32 edge)
{
    dl->AddLine(ImVec2(lx + 0.5f, y0 + 1.0f), ImVec2(lx + 0.5f, y1 - 1.0f), ui::kStrokeStrong, 1.0f);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), edge, ui::kRadiusSm);
}

// A card in the panel (a condition): its fill and edge are drawn behind its content once
// its height is known. Begin / End around the content; the cursor ends under it.
constexpr float kCardPad = 8.0f;
constexpr float kCardGap = 6.0f;
struct Card {
    ImDrawListSplitter split;
    ImVec2             p0;
    float              w = 0.0f;
};

void BeginCard(Card& k)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    k.p0 = ImGui::GetCursorScreenPos();
    k.w = ImGui::GetContentRegionAvail().x;
    k.split.Split(dl, 2);
    k.split.SetCurrentChannel(dl, 1);
    ImGui::SetCursorScreenPos(ImVec2(k.p0.x + kCardPad, k.p0.y + kCardPad));
    ImGui::BeginGroup();
}

void EndCard(Card& k, ImU32 fill, ImU32 edge)
{
    ImGui::EndGroup();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float bottom = ImGui::GetItemRectMax().y + kCardPad;
    k.split.SetCurrentChannel(dl, 0);
    dl->AddRectFilled(k.p0, ImVec2(k.p0.x + k.w, bottom), fill, ui::kRadiusMd);
    dl->AddRect(k.p0, ImVec2(k.p0.x + k.w, bottom), edge, ui::kRadiusMd);
    k.split.Merge(dl);
    ImGui::SetCursorScreenPos(ImVec2(k.p0.x, bottom));
    ImGui::Dummy(ImVec2(k.w, std::max(0.0f, kCardGap - ImGui::GetStyle().ItemSpacing.y)));
}

// Decision A, "aligned sentence": one phrase per row, its connector word right-aligned in a
// fixed column, every row's chips from one shared edge. A chip that does not fit beside the
// previous one wraps under the row's first chip (a row wraps as a whole, never a lone word).
struct Sentence {
    float x0 = 0.0f;       // the label column's left
    float chips_x = 0.0f;  // the chips' shared left edge
    float right = 0.0f;    // the right edge chips stay within
    bool  first = true;    // no chip on this row yet
};

float SentenceGap() { return ImGui::GetStyle().ItemSpacing.x; }

// The label column: as wide as the widest label of the inspector (the IF / AND tag's pad included).
float SentenceColumnWidth()
{
    float w = ImGui::CalcTextSize("AND").x + 8.0f;
    for (const char* s : {"reads", "from", "goes", "margin", "at", "of", "Offset", "Min length", "Cooldown"})
        w = std::max(w, ImGui::CalcTextSize(s).x);
    return w;
}

Sentence MakeSentence(float x0, float right)
{
    Sentence s;
    s.x0 = x0;
    s.chips_x = x0 + SentenceColumnWidth() + SentenceGap();
    s.right = std::max(s.chips_x + 20.0f, right);
    return s;
}

// Starts a row on the cursor's line: its label right-aligned in the column (an item, so it
// can be hovered). `badge`: drawn as the IF / AND tag.
void SentenceRow(Sentence& s, const char* label, bool badge = false)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fh = ImGui::GetFrameHeight();
    const float y = ImGui::GetCursorScreenPos().y;
    ImGui::SetCursorScreenPos(ImVec2(s.x0, y));
    ImGui::Dummy(ImVec2(s.chips_x - SentenceGap() - s.x0, fh));
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const float  xr = s.chips_x - SentenceGap();
    if (badge) {
        const float bx = xr - ts.x - 8.0f;
        dl->AddRectFilled(ImVec2(bx, y + 3.0f), ImVec2(xr, y + fh - 3.0f), ui::kBg, 4.0f);
        dl->AddRect(ImVec2(bx, y + 3.0f), ImVec2(xr, y + fh - 3.0f), ui::kStroke, 4.0f);
        dl->AddText(ImVec2(bx + 4.0f, y + (fh - ts.y) * 0.5f), ui::kMuted, label);
    } else {
        dl->AddText(ImVec2(xr - ts.x, y + (fh - ts.y) * 0.5f), ui::kMuted, label);
    }
    s.first = true;
}

// Puts the cursor where the row's next chip (w wide) goes: beside the previous one when it
// fits (`spacing` < 0 = the item spacing), else under the row's first chip.
void SentenceChip(Sentence& s, float w, float spacing = -1.0f)
{
    const float sp = spacing < 0.0f ? SentenceGap() : spacing;
    g_chip_right = s.right;
    if (s.first) {
        ImGui::SameLine(0.0f, SentenceGap());
        s.first = false;
    } else if (ImGui::GetItemRectMax().x + sp + w <= s.right) {
        ImGui::SameLine(0.0f, sp);
    } else {
        ImGui::SetCursorScreenPos(ImVec2(s.chips_x, ImGui::GetCursorScreenPos().y));
    }
}

// The width a number field takes with its unit.
float NumberFieldWidth(float field_w, const char* unit)
{
    return field_w + ((unit && unit[0]) ? 4.0f + ImGui::CalcTextSize(unit).x : 0.0f);
}

// ---- Strip -----------------------------------------------------------------------------------

// A lane's value range: the curve over the whole clip, plus the saved threshold and re-arm
// level (stable while a line is dragged), padded.
void LaneRange(const std::vector<double>& curve, const Condition* saved, double* lo, double* hi)
{
    double a = 0.0, b = 0.0;
    bool any = false;
    for (double v : curve) {
        if (!std::isfinite(v)) continue;
        if (!any) a = b = v;
        a = std::min(a, v);
        b = std::max(b, v);
        any = true;
    }
    if (saved) {
        const double m = std::max(0.0, saved->margin);
        const double re = saved->dir == Direction::Below ? saved->threshold + m : saved->threshold - m;
        for (double v : {saved->threshold, re}) {
            if (!std::isfinite(v)) continue;
            if (!any) a = b = v;
            a = std::min(a, v);
            b = std::max(b, v);
            any = true;
        }
    }
    if (!(b - a > 1e-9)) {
        a -= 0.5;
        b += 0.5;
    }
    const double pad = (b - a) * 0.08;
    *lo = a - pad;
    *hi = b + pad;
}

// The item under the playhead changed: a running number drag or typed value, a preview
// and a line drag belonged to the previous one (nothing of them is written).
void OnItemChanged(MediaItem* item)
{
    g_sel_item = item;
    g_sel = 0;
    g_drag = StripDrag::None;
    ClearEventSelection();
    ClearLastApplyResult();
    ui::DragNumberReset();
    TaggingCancelPreview();
}

void DashedHLine(ImDrawList* dl, float x0, float x1, float y, ImU32 col)
{
    for (float x = x0; x < x1; x += 7.0f) dl->AddLine(ImVec2(x, y), ImVec2(std::min(x + 4.0f, x1), y), col, 1.0f);
}

// Centred message lines over a rect.
void CenterLines(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* l1, ImU32 c1, const char* l2 = nullptr,
                 ImU32 c2 = ui::kMuted)
{
    const float th = ImGui::GetTextLineHeight();
    const float total = l2 ? th * 2.0f + 4.0f : th;
    float y = (a.y + b.y - total) * 0.5f;
    for (int k = 0; k < 2; ++k) {
        const char* l = k == 0 ? l1 : l2;
        if (!l) continue;
        const ImVec2 ts = ImGui::CalcTextSize(l);
        dl->AddText(ImVec2(std::max(a.x + 6.0f, (a.x + b.x - ts.x) * 0.5f), y), k == 0 ? c1 : c2, l);
        y += th + 4.0f;
    }
}

struct LaneGeom {
    int    cond = 0;
    float  y0 = 0.0f, y1 = 0.0f;
    double lo = 0.0, hi = 1.0;
    float  Y(double v) const
    {
        const double f = (std::min(std::max(v, lo), hi) - lo) / (hi - lo);
        return y1 - 3.0f - static_cast<float>(f) * (y1 - y0 - 6.0f);
    }
    double V(float y) const { return lo + static_cast<double>((y1 - 3.0f - y) / (y1 - y0 - 6.0f)) * (hi - lo); }
};

}  // namespace

float TaggingStripHeight(float client_h)
{
    if (g_strip_h < 0.0f) g_strip_h = LoadPrefFloat(kPrefStripHeight, kStripDefaultHeight);
    return std::min(std::max(g_strip_h, kTaggingStripMinHeight), StripMaxHeight(client_h));
}

float TaggingPanelFootprint(int client_w)
{
    if (!TaggingViewActive()) return 0.0f;
    // A narrow window keeps some room for the 3D view.
    const float w = std::min(PanelWidth(), std::max(160.0f, static_cast<float>(client_w) * 0.7f));
    return w + 2.0f * kTaggingGap;
}

bool TaggingGestureActive()
{
    return g_drag == StripDrag::Threshold || g_drag == StripDrag::Margin || g_drag == StripDrag::Event ||
           ui::DragNumberActive();
}

void TaggingAddEventAtPlayhead()
{
    const TaggingModel& m = GetTaggingModel();
    if (!EventsEditable(m)) return;
    const ItemRules& rules = TaggingShownRules();
    if (g_sel < 0 || g_sel >= static_cast<int>(rules.blocks.size())) return;
    double clip_t = 0.0;
    if (!FirstPassClipTime(m.map, Playhead(nullptr), &clip_t)) return;  // the playhead is off the clip's first pass
    AddUserEvent(g_sel, clip_t);
}

void TaggingDeleteSelectedEvent()
{
    const TaggingModel& m = GetTaggingModel();
    if (!EventsEditable(m)) return;
    if (const ShownEvent* e = SelectedEvent(m)) ToggleEvent(*e);
}

bool TaggingConsumeMenuRequest()
{
    const bool r = g_menu_request;
    g_menu_request = false;
    return r;
}

void TaggingEndGestures()
{
    if (GetTaggingModel().previewing) TaggingCommitPreview("RAV: Edit auto-tagging rule");
    g_drag = StripDrag::None;
    g_panel_drag = false;
    ui::DragNumberReset();
}

bool TaggingSelectedRuleBones(std::vector<int>* bones, unsigned int* colour)
{
    if (bones) bones->clear();
    if (!TaggingViewActive()) return false;
    const TaggingModel& m = GetTaggingModel();
    if (!m.file_loaded || m.bone_names.empty() || m.item != g_sel_item) return false;
    const ItemRules& rules = TaggingShownRules();
    if (g_sel < 0 || g_sel >= static_cast<int>(rules.blocks.size())) return false;
    const Block& blk = rules.blocks[static_cast<size_t>(g_sel)];
    if (bones)
        *bones = RuleBonesOnSkeleton(blk, m.role_to_bone, m.custom_role_to_bone, m.bone_names, m.bone_parents);
    if (colour) *colour = RuleColor(rules.blocks[static_cast<size_t>(g_sel)]);
    return true;
}

void DrawTaggingStrip(float x, float y, float w, float h)
{
    const TaggingModel& m = GetTaggingModel();
    if (m.item != g_sel_item) OnItemChanged(m.item);
    const ItemRules& rules = TaggingShownRules();
    if (g_sel >= static_cast<int>(rules.blocks.size())) g_sel = static_cast<int>(rules.blocks.size()) - 1;
    if (g_sel < 0) g_sel = 0;

    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(std::max(w, 160.0f), h), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::Col(ui::kSurface));
    ImGui::PushStyleColor(ImGuiCol_Border, ui::Col(ui::kStroke));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##tagstrip", nullptr, kFlags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        StripHeightGrip(g_strip_grip, "##tagstripgrip", &g_strip_h, h, kTaggingStripMinHeight,
                        StripMaxHeight(ImGui::GetIO().DisplaySize.y), kPrefStripHeight);

        bool playing = false;
        const double playhead = Playhead(&playing);
        const double fps = ProjectFps();
        const float th = ImGui::GetTextLineHeight();

        // ---- Header: timecode, the selected rule, Analyse ----
        const ImVec2 c0 = ImGui::GetCursorScreenPos();
        const float avail_w = ImGui::GetContentRegionAvail().x;
        const float head_h = ImGui::GetFrameHeight();
        char tc[32];
        FormatTime(playhead, tc, sizeof(tc));
        dl->AddText(ImVec2(c0.x, c0.y + (head_h - th) * 0.5f), ui::kMuted, tc);
        {
            const char* label = "Analyse";
            const float bw = ImGui::CalcTextSize(label).x + 20.0f;
            // Story 10-4: "+ Event [E]" adds a user event at the playhead on the selected rule.
            {
                const char* key = ShortcutKeyLabel(kShortcutTagAddEvent);
                const float ew = ImGui::CalcTextSize("+ Event").x + ImGui::CalcTextSize(key).x + 30.0f;
                const ImVec2 ep(c0.x + avail_w - bw - 8.0f - ew, c0.y);
                ImGui::SetCursorScreenPos(ep);
                const bool can_add = EventsEditable(m) && g_sel < static_cast<int>(rules.blocks.size());
                if (!can_add) ImGui::BeginDisabled();
                if (ui::SolidButton("##tagaddev", ImVec2(ew, 0.0f))) Later([]() { TaggingAddEventAtPlayhead(); });
                if (!can_add) ImGui::EndDisabled();
                const float ty = ep.y + (head_h - th) * 0.5f;
                dl->AddText(ImVec2(ep.x + 8.0f, ty), can_add ? ui::kText : ui::kFaint, "+ Event");
                ui::KeyCap(dl, ImVec2(ep.x + 14.0f + ImGui::CalcTextSize("+ Event").x, ty - 1.5f), key,
                           can_add ? ui::kText : ui::kFaint);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Adds your own event at the playhead on the selected rule.\n"
                                      "Double-click a rule's row to add one there.");
            }
            ImGui::SetCursorScreenPos(ImVec2(c0.x + avail_w - bw, c0.y));
            const bool can = m.item && m.has_record && m.missing_count == 0 && m.file_loaded && !rules.blocks.empty();
            if (!can) ImGui::BeginDisabled();
            if (ui::SolidButton("Analyse##taganalyse", ImVec2(bw, 0.0f))) Later([]() { TaggingAnalyse(); });
            if (!can) ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Proposes this item's thresholds from the clip.\nFixed (locked) conditions keep theirs.");
        }

        // ---- The body: a gutter of labels, then the lanes over the view window ----
        const float body_top = c0.y + head_h + 6.0f;
        const float body_bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y - 6.0f;
        const float gutter = std::min(160.0f, std::max(90.0f, avail_w * 0.22f));
        const float lane_x = c0.x + gutter;
        const float lane_w = std::max(20.0f, avail_w - gutter);
        const float ruler_h = std::floor(ImGui::GetFontSize()) + kStripRulerTickRoom;
        const float rows_top = body_top + ruler_h;
        const ImVec2 area0(c0.x, body_top);
        const ImVec2 area1(c0.x + avail_w, std::max(body_top + 10.0f, body_bottom));

        // One invisible button over the body: it takes the clicks (seek, select, line drags).
        ImGui::SetCursorScreenPos(area0);
        ImGui::InvisibleButton("##tagarea", ImVec2(area1.x - area0.x, area1.y - area0.y));
        const bool area_active = ImGui::IsItemActive();
        const bool area_hovered = ImGui::IsItemHovered();
        const bool area_pressed = ImGui::IsItemActivated();
        const bool area_released = ImGui::IsItemDeactivated();
        const ImVec2 mouse = ImGui::GetIO().MousePos;

        const char* msg1 = nullptr;
        const char* msg2 = nullptr;
        ImU32 msg_col = ui::kFaint;
        char miss_line[160];
        std::string miss_names;
        if (!m.item) {
            msg1 = "No animation item under the playhead";
        } else if (!m.file_loaded) {
            msg1 = "The animation file did not load: no signal to draw.";
        } else if (m.unreadable) {
            msg1 = "This item's rules could not be read.";
            msg2 = "Pick a preset in the panel to replace them.";
        } else if (m.missing_count > 0) {
            std::snprintf(miss_line, sizeof(miss_line), "%d role%s no bone on this skeleton: no signal to draw.",
                          m.missing_count, m.missing_count == 1 ? " has" : "s have");
            msg1 = miss_line;
            msg_col = kBadText;
            miss_names = "Missing: " + m.missing + ". Commit skips this item until they have a bone.";
            msg2 = miss_names.c_str();
        } else if (!m.sample_error.empty()) {
            msg1 = m.sample_error.c_str();
        } else if (rules.blocks.empty()) {
            msg1 = "No rule. Add one in the panel (+ Rule), or pick a preset.";
        }

        if (!m.item || !(m.map.item_len > 0.0)) {
            StripViewReset(g_view);
            dl->AddRectFilled(ImVec2(lane_x, rows_top), area1, ui::kBg, ui::kRadiusSm);
            CenterLines(dl, ImVec2(lane_x, rows_top), area1, msg1 ? msg1 : "No animation item under the playhead",
                        msg_col, msg2);
        } else {
            const double item_start = m.map.item_pos;
            const double item_end = m.map.item_pos + m.map.item_len;
            const bool hold = area_active && g_drag != StripDrag::None;
            const StripWindow win = StripViewUpdate(g_view, item_start, item_end, playhead, playing, fps, hold,
                                                    ImGui::IsWindowHovered(), mouse.x, lane_x, lane_w);
            auto x_of = [&](double t) { return StripX(win, lane_x, lane_w, t); };
            const double t_mouse = StripTimeAt(win, lane_x, lane_w, mouse.x);
            const StripRuler ruler = StripRulerFor(win, lane_w, fps);
            DrawStripRuler(dl, win, ruler, lane_x, lane_w, rows_top, ruler_h, fps);
            // Story 10-4: RAV plays the clip once then holds its last frame: the strip shows the
            // clip's first pass only (as Apply writes its markers).
            std::vector<ClipPass> passes;
            {
                ClipPass fp;
                if (FirstClipPass(m.map, &fp)) passes.push_back(fp);
            }
            const float zx0 = x_of(item_start), zx1 = x_of(item_end);

            // The item's zone in the lanes area (outside it: dimmed).
            auto dim_outside = [&](float ya, float yb) {
                constexpr ImU32 kOutsideDim = IM_COL32(0x12, 0x13, 0x17, 0xA6);
                if (zx0 > lane_x) dl->AddRectFilled(ImVec2(lane_x, ya), ImVec2(std::min(zx0, lane_x + lane_w), yb), kOutsideDim);
                if (zx1 < lane_x + lane_w)
                    dl->AddRectFilled(ImVec2(std::max(zx1, lane_x), ya), ImVec2(lane_x + lane_w, yb), kOutsideDim);
            };

            const bool can_draw = msg1 == nullptr && m.detected;
            if (!can_draw) {
                dl->AddRectFilled(ImVec2(lane_x, rows_top), area1, ui::kBg, ui::kRadiusSm);
                if (m.missing_count > 0) dl->AddRect(ImVec2(lane_x, rows_top), area1, WithAlpha(kBad, 0x99), ui::kRadiusSm);
                CenterLines(dl, ImVec2(lane_x, rows_top), area1, msg1 ? msg1 : "Detecting...", msg_col, msg2);
                if (area_pressed) g_drag = StripDrag::Scrub;
            } else {
                // ---- Notify rows: one per rule, its markers ----
                const int nb = static_cast<int>(rules.blocks.size());
                const ShownEvent* sel_ev = SelectedEvent(m);
                const float row_h = std::max(16.0f, th + 6.0f);
                // 10-3 fb-2: every notify row and condition lane is one boxed track (a header
                // cell in the gutter, its lane), kTrackGap apart. A shortened header keeps its
                // full text for the tooltip.
                constexpr float kTrackGap = 3.0f;
                struct GutterTip {
                    float       y0, y1;
                    std::string text;
                };
                std::vector<GutterTip> gutter_tips;
                std::vector<float> row_y(static_cast<size_t>(nb));
                for (int b = 0; b < nb; ++b) {
                    const Block& blk = rules.blocks[static_cast<size_t>(b)];
                    const float ry = rows_top + kTrackGap + static_cast<float>(b) * (row_h + kTrackGap);
                    row_y[static_cast<size_t>(b)] = ry;
                    const bool sel = b == g_sel;
                    TrackFill(dl, c0.x, lane_x, lane_x + lane_w, ry, ry + row_h, ui::kBg);
                    if (sel)
                        dl->AddRectFilled(ImVec2(lane_x, ry), ImVec2(lane_x + lane_w, ry + row_h), IM_COL32(0x5B, 0x8C, 0xFF, 0x1A),
                                          ui::kRadiusSm, ImDrawFlags_RoundCornersRight);
                    dl->PushClipRect(ImVec2(lane_x, ry), ImVec2(lane_x + lane_w, ry + row_h), true);
                    // Story 10-4: the rule's events (detected, suppressed, orphan suppressions,
                    // the user's own), on the clip's first pass.
                    {
                        const ImU32 col = RuleColor(blk);
                        const bool dragging = g_drag == StripDrag::Event && g_drag_moved && g_drag_ev.block == b;
                        for (const ShownEvent& e : m.events) {
                            if (e.block != b) continue;
                            double pt = 0.0;
                            if (!FirstPassProjectTime(m.map, e.t, &pt) || pt < win.v0 || pt > win.v1) continue;
                            const bool is_sel = sel_ev && sel_ev->block == e.block && sel_ev->t == e.t && sel_ev->kind == e.kind;
                            const bool moving = dragging && e.kind == g_drag_ev.kind && e.t == g_drag_ev.t;
                            const float ex = std::floor(x_of(pt)) + 0.5f;
                            DrawEventMark(dl, e, ex, ry, row_h, moving ? WithAlpha(col, 0x40) : col, is_sel && !moving);
                        }
                        if (dragging) {  // where it lands: the user's own event
                            double pt = 0.0;
                            if (FirstPassProjectTime(m.map, g_drag_ev_to, &pt)) {
                                ShownEvent ghost = g_drag_ev;
                                ghost.kind = ShownKind::User;
                                DrawEventMark(dl, ghost, std::floor(x_of(pt)) + 0.5f, ry, row_h, col, true);
                            }
                        }
                    }
                    dl->PopClipRect();
                    dim_outside(ry, ry + row_h);  // over the marks too (outside the item: dimmed)
                    TrackEdge(dl, c0.x, lane_x, lane_x + lane_w, ry, ry + row_h, sel ? ui::kAccentLine : ui::kStroke);
                    if (sel)  // the selected rule: a stripe in its colour on its header's left
                        RuleStripe(dl, ImVec2(c0.x, ry), ImVec2(lane_x + lane_w, ry + row_h), RuleColor(blk), ui::kRadiusSm);
                    dl->AddCircleFilled(ImVec2(c0.x + 11.0f, ry + row_h * 0.5f), 3.5f,
                                        blk.enabled ? RuleColor(blk) : WithAlpha(RuleColor(blk), 0x66), 12);
                    const std::string title = RuleTitle(blk);
                    if (EllipsisText(dl, ImVec2(c0.x + 20.0f, ry + (row_h - th) * 0.5f), lane_x - 5.0f,
                                     blk.enabled ? (sel ? ui::kText : ui::kMuted) : ui::kFaint, title))
                        gutter_tips.push_back({ry, ry + row_h, title});
                }
                const float rows_bottom = rows_top + kTrackGap + static_cast<float>(nb) * (row_h + kTrackGap);

                // ---- The selected rule's lanes ----
                std::vector<LaneGeom> lanes;
                const Block* sel_blk = (g_sel < nb) ? &rules.blocks[static_cast<size_t>(g_sel)] : nullptr;
                const BlockTrace* bt = (sel_blk && g_sel < static_cast<int>(m.trace.blocks.size()))
                                           ? &m.trace.blocks[static_cast<size_t>(g_sel)]
                                           : nullptr;
                const float lanes_top = rows_bottom + 4.0f;
                if (sel_blk && bt && bt->ran && lanes_top < area1.y - 20.0f) {
                    const int nc = static_cast<int>(sel_blk->conditions.size());
                    const float gap = kTrackGap;
                    const float lh = std::max(28.0f, (area1.y - lanes_top - gap * static_cast<float>(nc - 1)) / nc);
                    const Block* saved_blk =
                        g_sel < static_cast<int>(m.rules.blocks.size()) ? &m.rules.blocks[static_cast<size_t>(g_sel)] : nullptr;
                    const ImU32 rc = RuleColor(*sel_blk);
                    const double rate_hz = m.trace.rate_hz > 0.0 ? m.trace.rate_hz : 240.0;
                    const double item_rate = m.map.rate > 0.0 ? m.map.rate : 1.0;
                    double clip_now = 0.0;
                    const bool now_in = FirstPassClipTime(m.map, playhead, &clip_now);
                    for (int c = 0; c < nc; ++c) {
                        LaneGeom L;
                        L.cond = c;
                        L.y0 = lanes_top + static_cast<float>(c) * (lh + gap);
                        L.y1 = std::min(L.y0 + lh, area1.y);
                        if (L.y1 - L.y0 < 12.0f) break;
                        const Condition& cd = sel_blk->conditions[static_cast<size_t>(c)];
                        const Condition* saved =
                            (saved_blk && c < static_cast<int>(saved_blk->conditions.size())) ? &saved_blk->conditions[static_cast<size_t>(c)]
                                                                                             : &cd;
                        const std::vector<double>& curve = bt->curves[static_cast<size_t>(c)];
                        const std::vector<char>& hold = bt->holds[static_cast<size_t>(c)];
                        LaneRange(curve, saved, &L.lo, &L.hi);
                        lanes.push_back(L);

                        TrackFill(dl, c0.x, lane_x, lane_x + lane_w, L.y0, L.y1, ui::kBg);
                        dl->PushClipRect(ImVec2(lane_x, L.y0), ImVec2(lane_x + lane_w, L.y1), true);
                        // Column by column over each pass of the clip: the whole-rule shading,
                        // then the curve (its min / max in the column), bright where it holds.
                        const int n = static_cast<int>(curve.size());
                        for (const ClipPass& p : passes) {
                            const double pa = std::max(p.p0, win.v0), pb = std::min(p.p1, win.v1);
                            if (!(pb > pa) || n < 1) continue;
                            const int xa = static_cast<int>(std::floor(x_of(pa)));
                            const int xb = static_cast<int>(std::ceil(x_of(pb)));
                            float shade_from = -1.0f;
                            bool have_prev = false;
                            float prev_y = 0.0f;
                            for (int px = xa; px < xb; ++px) {
                                const double ta = std::max(pa, StripTimeAt(win, lane_x, lane_w, static_cast<float>(px)));
                                const double tb = std::min(pb, StripTimeAt(win, lane_x, lane_w, static_cast<float>(px + 1)));
                                if (!(tb > ta)) continue;
                                const double sa = SamplePosInPass(p, ta, item_rate, rate_hz);
                                const double sb = SamplePosInPass(p, tb, item_rate, rate_hz);
                                const double va = InterpSample(curve, sa), vb = InterpSample(curve, sb);
                                double lo = std::min(va, vb), hi = std::max(va, vb);
                                const int i_lo = std::max(0, static_cast<int>(std::ceil(sa)));
                                const int i_hi = std::min(n - 1, static_cast<int>(std::floor(sb)));
                                for (int i = i_lo; i <= i_hi; ++i) {
                                    lo = std::min(lo, curve[static_cast<size_t>(i)]);
                                    hi = std::max(hi, curve[static_cast<size_t>(i)]);
                                }
                                const int h_lo = std::max(0, std::min(n - 1, static_cast<int>(std::floor(sa))));
                                const int h_hi = std::max(0, std::min(n - 1, static_cast<int>(std::ceil(sb))));
                                bool any_hold = false, any_active = false;
                                for (int i = h_lo; i <= h_hi; ++i) {
                                    if (static_cast<size_t>(i) < hold.size() && hold[static_cast<size_t>(i)]) any_hold = true;
                                    if (static_cast<size_t>(i) < bt->active.size() && bt->active[static_cast<size_t>(i)])
                                        any_active = true;
                                }
                                const float fx = static_cast<float>(px);
                                if (any_active && shade_from < 0.0f) shade_from = fx;
                                if (!any_active && shade_from >= 0.0f) {
                                    dl->AddRectFilled(ImVec2(shade_from, L.y0), ImVec2(fx, L.y1), WithAlpha(rc, 0x24));
                                    shade_from = -1.0f;
                                }
                                const ImU32 ink = any_hold ? ui::kText : ui::kFaint;
                                const float ya = L.Y(va), yb = L.Y(vb);
                                if (have_prev) dl->AddLine(ImVec2(fx, prev_y), ImVec2(fx + 0.5f, ya), ink, 1.3f);
                                dl->AddLine(ImVec2(fx + 0.5f, L.Y(hi)), ImVec2(fx + 0.5f, L.Y(lo) + 0.01f), ink, 1.3f);
                                dl->AddLine(ImVec2(fx + 0.5f, ya), ImVec2(fx + 1.0f, yb), ink, 1.3f);
                                prev_y = yb;
                                have_prev = true;
                            }
                            if (shade_from >= 0.0f)
                                dl->AddRectFilled(ImVec2(shade_from, L.y0), ImVec2(static_cast<float>(xb), L.y1), WithAlpha(rc, 0x24));
                        }
                        // Threshold (solid) and re-arm level (dashed), the band between them.
                        const float yt = L.Y(cd.threshold);
                        const double m_ = std::max(0.0, cd.margin);
                        const bool hot = (g_drag == StripDrag::Threshold || g_drag == StripDrag::Margin) &&
                                         g_drag_block == g_sel && g_drag_cond == c;
                        if (m_ > 0.0) {
                            const float yr = L.Y(c < static_cast<int>(bt->rearm.size()) ? bt->rearm[static_cast<size_t>(c)]
                                                                                        : cd.threshold + m_);
                            dl->AddRectFilled(ImVec2(lane_x, std::min(yt, yr)), ImVec2(lane_x + lane_w, std::max(yt, yr)),
                                              WithAlpha(rc, 0x14));
                            DashedHLine(dl, lane_x, lane_x + lane_w, std::floor(yr) + 0.5f,
                                        WithAlpha(rc, hot && g_drag == StripDrag::Margin ? 0xFF : 0x99));
                        }
                        dl->AddLine(ImVec2(lane_x, yt), ImVec2(lane_x + lane_w, yt), rc,
                                    hot && g_drag == StripDrag::Threshold ? 2.5f : 1.5f);
                        dl->PopClipRect();
                        dim_outside(L.y0, L.y1);
                        TrackEdge(dl, c0.x, lane_x, lane_x + lane_w, L.y0, L.y1, ui::kStroke);
                        dl->AddRectFilled(ImVec2(lane_x - 2.0f, yt - 4.0f), ImVec2(lane_x + 6.0f, yt + 4.0f), rc, 2.0f);

                        // The gutter (the track's header cell): the signal's name, its
                        // condition, its value now; shortened with "..." to the cell.
                        dl->PushClipRect(ImVec2(c0.x, L.y0), ImVec2(lane_x - 4.0f, L.y1), true);
                        const float gx = c0.x + 6.0f, gx1 = lane_x - 5.0f;
                        const std::string name = std::string(c ? "AND " : "IF ") +
                                                 SignalName(cd.signal, SignalBoneLabel(cd.signal)) +
                                                 ((sel_blk->landing == Landing::PeakOf && sel_blk->peak_condition == c)
                                                      ? (sel_blk->peak_max ? "  (peak)" : "  (low)")
                                                      : "");
                        bool cut = EllipsisText(dl, ImVec2(gx, L.y0 + 2.0f), gx1, ui::kText, name);
                        std::string cond_text = std::string(cd.dir == Direction::Below ? "< " : "> ") +
                                                FormatDisplay(cd.signal, cd.threshold);
                        if (m_ > 0.0) cond_text += "  margin " + FormatDisplay(cd.signal, m_);
                        if (L.y1 - L.y0 > th * 2.0f + 4.0f) {
                            if (EllipsisText(dl, ImVec2(gx, L.y0 + 3.0f + th), gx1, rc, cond_text)) cut = true;
                        } else {
                            cut = true;  // the condition line has no room: the tooltip shows it
                        }
                        std::string tip = name + "\n" + cond_text;
                        if (now_in && L.y1 - L.y0 > th * 3.0f + 6.0f && !curve.empty()) {
                            const double pos = clip_now * rate_hz;
                            const double v_now = InterpSample(curve, pos);
                            const size_t hi_ = static_cast<size_t>(std::min<double>(static_cast<double>(curve.size() - 1),
                                                                                    std::max(0.0, std::round(pos))));
                            const bool in_now = hi_ < hold.size() && hold[hi_];
                            const std::string now_text = "now " + FormatDisplay(cd.signal, v_now);
                            if (EllipsisText(dl, ImVec2(gx, L.y0 + 4.0f + 2.0f * th), gx1, in_now ? ui::kText : ui::kFaint,
                                             now_text))
                                cut = true;
                            tip += "\n" + now_text;
                        }
                        if (cut) gutter_tips.push_back({L.y0, L.y1, tip});
                        dl->PopClipRect();
                    }
                } else if (sel_blk && lanes_top >= area1.y - 20.0f) {
                    // The notify rows leave no room for the lanes.
                    if (area1.y - rows_bottom > ImGui::GetTextLineHeight())
                        CenterLines(dl, ImVec2(lane_x, rows_bottom), area1, "Enlarge the strip to see this rule's signals",
                                    ui::kFaint);
                } else if (sel_blk && !sel_blk->enabled) {
                    CenterLines(dl, ImVec2(lane_x, lanes_top), area1, "This rule is off: switch it on to see its signals.",
                                ui::kFaint);
                }

                // ---- Story 10-4 follow-up: the selected rule's events through its lanes ----
                // Each event draws a thin line in the rule's colour from its notify row down
                // through every condition lane to the strip's bottom, so the user sees where it
                // crosses each curve: suppressed faint, orphan dashed, the selected one brighter.
                // A dragged event's line follows the drop time (Esc: back where it was).
                if (sel_blk && !lanes.empty() && g_sel >= 0 && g_sel < nb) {
                    const ImU32 rc = RuleColor(*sel_blk);
                    const float ly0 = row_y[static_cast<size_t>(g_sel)] + row_h;
                    const float ly1 = area1.y;
                    const bool dragging = g_drag == StripDrag::Event && g_drag_moved && g_drag_ev.block == g_sel;
                    auto vline = [&](float x, ImU32 col, float thick, bool dashed) {
                        if (dashed) {
                            for (float y = ly0; y < ly1; y += 5.0f)
                                dl->AddLine(ImVec2(x, y), ImVec2(x, std::min(y + 2.5f, ly1)), col, thick);
                        } else {
                            dl->AddLine(ImVec2(x, ly0), ImVec2(x, ly1), col, thick);
                        }
                    };
                    dl->PushClipRect(ImVec2(lane_x, ly0), ImVec2(lane_x + lane_w, ly1), true);
                    for (const ShownEvent& e : m.events) {
                        if (e.block != g_sel) continue;
                        double pt = 0.0;
                        if (!FirstPassProjectTime(m.map, e.t, &pt) || pt < win.v0 || pt > win.v1) continue;
                        const bool is_sel = sel_ev && sel_ev->block == e.block && sel_ev->t == e.t && sel_ev->kind == e.kind;
                        const bool moving = dragging && e.kind == g_drag_ev.kind && e.t == g_drag_ev.t;
                        const float ex = std::floor(x_of(pt)) + 0.5f;
                        if (moving) {
                            vline(ex, WithAlpha(rc, 0x30), 1.0f, true);  // where it was
                        } else if (e.kind == ShownKind::Orphan) {
                            vline(ex, WithAlpha(rc, is_sel ? 0xC0 : 0x66), 1.0f, true);
                        } else if (e.kind == ShownKind::Suppressed) {
                            vline(ex, WithAlpha(rc, is_sel ? 0x99 : 0x38), 1.0f, false);
                        } else {
                            vline(ex, WithAlpha(rc, is_sel ? 0xF0 : 0x80), 1.0f, false);
                        }
                    }
                    if (dragging) {
                        double pt = 0.0;
                        if (FirstPassProjectTime(m.map, g_drag_ev_to, &pt) && pt >= win.v0 && pt <= win.v1)
                            vline(std::floor(x_of(pt)) + 0.5f, WithAlpha(rc, 0xF0), 1.0f, false);
                    }
                    dl->PopClipRect();
                }

                // ---- Mouse: the lines, the rows, the scrub ----
                auto line_at = [&](float mx, float my, int* cond, bool* margin) {
                    if (mx < lane_x || mx > lane_x + lane_w || !sel_blk) return false;
                    for (const LaneGeom& L : lanes) {
                        if (my < L.y0 || my > L.y1) continue;
                        const Condition& cd = sel_blk->conditions[static_cast<size_t>(L.cond)];
                        const float yt = L.Y(cd.threshold);
                        const double mg = std::max(0.0, cd.margin);
                        const float yr = L.Y(cd.dir == Direction::Below ? cd.threshold + mg : cd.threshold - mg);
                        if (std::fabs(my - yt) <= 5.0f) {
                            *cond = L.cond;
                            *margin = false;
                            return true;
                        }
                        if (mg > 0.0 && std::fabs(my - yr) <= 5.0f) {
                            *cond = L.cond;
                            *margin = true;
                            return true;
                        }
                    }
                    return false;
                };
                // Story 10-4: the notify row under the mouse (-1 = none) and its nearest event within 7 px.
                auto row_at = [&](float my) {
                    for (int b = 0; b < nb; ++b) {
                        const float ry = row_y[static_cast<size_t>(b)];
                        if (my >= ry && my <= ry + row_h) return b;
                    }
                    return -1;
                };
                auto event_at = [&](float mx, float my) -> const ShownEvent* {
                    const int b = row_at(my);
                    if (b < 0 || mx < lane_x || mx > lane_x + lane_w) return nullptr;
                    const ShownEvent* best = nullptr;
                    float best_d = 7.0f;
                    for (const ShownEvent& e : m.events) {
                        double pt = 0.0;
                        if (e.block != b || !FirstPassProjectTime(m.map, e.t, &pt)) continue;
                        const float d = std::fabs(x_of(pt) - mx);
                        if (d <= best_d) {
                            best_d = d;
                            best = &e;
                        }
                    }
                    return best;
                };
                const bool events_ok = EventsEditable(m) && !m.previewing;
                if (area_pressed) {
                    g_drag = StripDrag::Scrub;
                    g_drag_moved = false;
                    int cond = -1;
                    bool margin = false;
                    const ShownEvent* hit = events_ok ? event_at(mouse.x, mouse.y) : nullptr;
                    if (hit) {
                        // An event: select it; a detection or a user event can be dragged.
                        SelectEvent(*hit);
                        g_drag = (hit->kind == ShownKind::Detected || hit->kind == ShownKind::User) ? StripDrag::Event
                                                                                                     : StripDrag::None;
                        g_drag_ev = *hit;
                        g_drag_ev_entry_t = EntryTime(*hit);
                        g_drag_ev_to = hit->t;
                        g_drag_press_x = mouse.x;
                    } else if (line_at(mouse.x, mouse.y, &cond, &margin)) {
                        g_drag = margin ? StripDrag::Margin : StripDrag::Threshold;
                        g_drag_block = g_sel;
                        g_drag_cond = cond;
                    } else {
                        ClearEventSelection();  // a click elsewhere: back to the rule
                        const int b = row_at(mouse.y);
                        if (b >= 0) {
                            g_sel = b;
                            if (mouse.x < lane_x) g_drag = StripDrag::None;  // the row's label: select only
                        }
                    }
                }
                // Story 10-4: dragging an event (past 2 px) moves it; Esc cancels; release writes.
                if (area_active && g_drag == StripDrag::Event) {
                    if (!g_drag_moved && std::fabs(mouse.x - g_drag_press_x) > 2.0f) g_drag_moved = true;
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                        g_drag = StripDrag::None;  // inert until the release
                        g_drag_moved = false;
                    } else if (g_drag_moved) {
                        double to = 0.0;
                        if (ClipTimeForDrop(m.map, t_mouse, &to) && to != g_drag_ev_to) {
                            g_drag_ev_to = to;
                            // The animation follows the dragged event (the playhead goes to where it lands).
                            double pt = 0.0;
                            if (FirstPassProjectTime(m.map, to, &pt)) QueueVideoSeek(pt);
                        }
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    }
                }
                if (area_released && g_drag == StripDrag::Event) {
                    if (g_drag_moved && events_ok) MoveEvent(g_drag_ev, g_drag_ev_entry_t, g_drag_ev_to);
                    g_drag = StripDrag::None;
                    g_drag_moved = false;
                }
                // Right-click: suppress / restore / delete. Double-click on a row: a user event there.
                if (area_hovered && events_ok && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    if (const ShownEvent* hit = event_at(mouse.x, mouse.y)) {
                        SelectEvent(*hit);
                        ToggleEvent(*hit);
                    }
                }
                if (area_hovered && events_ok && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && mouse.x >= lane_x &&
                    !event_at(mouse.x, mouse.y)) {
                    const int b = row_at(mouse.y);
                    double clip_t = 0.0;
                    if (b >= 0 && FirstPassClipTime(m.map, t_mouse, &clip_t)) {
                        g_sel = b;
                        AddUserEvent(b, clip_t);
                    }
                }
                if (area_active && (g_drag == StripDrag::Threshold || g_drag == StripDrag::Margin)) {
                    if (std::fabs(ImGui::GetIO().MouseDelta.y) > 0.0f) g_drag_moved = true;
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                        Later([]() { TaggingCancelPreview(); });
                        g_drag = StripDrag::None;  // inert until the release
                    } else if (g_drag_moved) {
                        for (const LaneGeom& L : lanes) {
                            if (L.cond != g_drag_cond || g_drag_block != g_sel) continue;
                            ItemRules p = TaggingShownRules();
                            Condition* cd = CondAt(p, g_drag_block, g_drag_cond);
                            if (!cd) break;
                            const SignalUnit u = UnitOf(cd->signal);
                            const double k = std::pow(10.0, UnitDecimals(u));
                            const double v = FromDisplay(cd->signal,
                                                         std::round(ToDisplay(cd->signal, L.V(mouse.y)) * k) / k);
                            if (g_drag == StripDrag::Threshold) {
                                cd->threshold = v;
                            } else {
                                const double d = std::fabs(v - cd->threshold);
                                cd->margin = FromDisplay(cd->signal, std::round(ToDisplay(cd->signal, d) * k) / k);
                            }
                            Later([p]() { TaggingPreview(p); });
                            break;
                        }
                    }
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                }
                if (area_released && (g_drag == StripDrag::Threshold || g_drag == StripDrag::Margin)) {
                    if (g_drag_moved && GetTaggingModel().previewing) {
                        const Block* blk = (g_drag_block < nb) ? &rules.blocks[static_cast<size_t>(g_drag_block)] : nullptr;
                        const std::string desc = std::string(g_drag == StripDrag::Threshold ? "RAV: Set threshold" : "RAV: Set margin") +
                                                 (blk ? " (" + RuleTitle(*blk) + ")" : "");
                        Later([desc]() { TaggingCommitPreview(desc.c_str()); });
                    } else {
                        Later([]() { TaggingCancelPreview(); });
                    }
                    g_drag = StripDrag::None;
                }
                if (area_hovered && !area_active) {
                    int cond = -1;
                    bool margin = false;
                    if (events_ok && event_at(mouse.x, mouse.y)) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                            ImGui::SetTooltip("Click: select  " RAV_DOT "  Drag: make it yours at a new time\n"
                                              "Right-click / Del: suppress, restore or delete");
                    } else if (line_at(mouse.x, mouse.y, &cond, &margin)) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                    } else if (mouse.x < lane_x) {
                        // 10-3 fb-2: a shortened header cell shows its full text.
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                            for (const GutterTip& g : gutter_tips)
                                if (mouse.y >= g.y0 && mouse.y <= g.y1) {
                                    ImGui::SetTooltip("%s", g.text.c_str());
                                    break;
                                }
                    } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                        ImGui::SetTooltip("Click or drag to move the playhead\nDrag a threshold line (dashed: the re-arm "
                                          "level) to tune it\nDouble-click a rule's row: add an event there\n"
                                          "Alt+wheel: zoom  " RAV_DOT "  Shift+wheel: scroll");
                    }
                }
            }

            // The playhead, over everything.
            if (playhead >= win.v0 && playhead <= win.v1) {
                const float px = x_of(playhead);
                dl->AddRectFilled(ImVec2(px - 1.0f, rows_top), ImVec2(px + 1.0f, area1.y), kPlayhead);
            }
            // Scrub: click / drag anywhere in the lanes (not on a line, not on a row's label).
            // The playhead stays on this item (its end is exclusive: just before it), so a click
            // beside the clip never switches the view to another item or to none.
            const double t_scrub = std::min(std::max(t_mouse, item_start), std::max(item_start, item_end - 1.0e-6));
            StripScrub(g_view, area_active && g_drag == StripDrag::Scrub && mouse.x >= lane_x, area_pressed, mouse.x,
                       t_scrub, &QueueVideoSeek);
        }
        if (!area_active && g_drag == StripDrag::Scrub) g_drag = StripDrag::None;
        if (!area_active && g_drag == StripDrag::Event) g_drag = StripDrag::None;  // the press was lost: nothing written
        if (!area_active && (g_drag == StripDrag::Threshold || g_drag == StripDrag::Margin)) {
            Later([]() { TaggingCancelPreview(); });  // the press was lost (the item went away): nothing written
            g_drag = StripDrag::None;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    RunLater();
}

namespace {

// ---- Panel: header -----------------------------------------------------------------------------

// ---- Panel: the preset field and its menu (story 10-3b) ---------------------------------------
// DM-XYZ-Pad's preset menu (lib/ui/preset_menu.lua) as an ImGui popup: search, Factory / User
// cascade, rows (click selects, double-click / Enter loads, right-click menu, F2 / Del / Esc),
// Save / Save as, the amber in-place confirm, the status line, the User folder with Copy, and
// Import / Export. Its logic is pure (preset_menu_model.h); disk actions run through Later and
// then read the files again. Only Load, Save / Save as, Update and Keep write the item (one undo
// point each); the files are never undo state.

PresetMenuState g_pm;
char g_pm_query[128] = {};
char g_pm_name[256] = {};
bool g_pm_focus_search = false;  // the search box takes the keyboard on the next frame
bool g_pm_focus_name = false;    // ...the name field
bool g_pm_search_active = false; // the search box had the keyboard last frame (Del edits its text)
bool g_pm_close = false;         // close the popup on the next frame (after a Load)
bool g_pm_arrowed = false;       // Up / Down this frame: the walked folder's submenu opens
int  g_pm_drawn_frame = -10;     // the last frame the field (and so the popup) was drawn
// The item a Save / Save as confirm or name field began on: confirmed on another item (the
// playhead moved meanwhile), nothing is saved.
MediaItem* g_pm_item = nullptr;

// Import / Export: the native pickers run a modal loop, so they run from the window procedure
// (TaggingRunPendingDialog), never inside the frame.
struct PendingDialog {
    enum class Kind { None, Import, Export, RolesImport, RolesExport };  // Roles*: story 10-3f
    Kind        kind = Kind::None;
    std::string id;
    std::string name;
    std::vector<std::string> bones;  // Roles*: the skeleton the Skeleton & roles window is open on
    std::vector<std::string> keys;   // RolesExport: the role keys the item's rules read
};
PendingDialog g_dialog;

// Story 10-3f -- the Skeleton & roles window's Import / Export (defined with the window).
void RolesRunDialog(HWND__* owner, const PendingDialog& d);

const FileDialogFilter kPresetFilter = {L"ReaAnimViewer preset (*.ravpreset)", L"*.ravpreset", L"ravpreset"};

void ResetPresetMenu()
{
    g_pm = PresetMenuState{};
    g_pm_query[0] = '\0';
    g_pm_name[0] = '\0';
    g_pm_focus_search = false;
    g_pm_focus_name = false;
    g_pm_search_active = false;
    g_pm_close = false;
    g_pm_item = nullptr;
}

constexpr char kItemChanged[] = "The item under the playhead changed. Nothing has been saved.";

// False (and the menu back to its list, saying so) when the item is no longer the one the
// Save / Save as began on.
bool PmSameItem()
{
    if (GetTaggingModel().item == g_pm_item) return true;
    g_pm.mode = PresetMenuMode::List;
    g_pm.confirm = PresetConfirm::None;
    g_pm.confirm_clash = false;
    g_pm.target_id.clear();
    g_pm.target_name.clear();
    g_pm.status = kItemChanged;
    return false;
}

const PresetInfo* PresetById(const std::string& id)
{
    for (const PresetInfo& p : CachedPresets())
        if (p.id == id) return &p;
    return nullptr;
}

// After a disk action: the list and the field read the files again.
void PresetFilesChanged()
{
    CachedPresets(/*refresh=*/true);
    TaggingPresetFilesChanged();
}

void PmStatus(const std::string& s)
{
    g_pm.status = s;
}

void PmStartNaming()
{
    g_pm_item = GetTaggingModel().item;
    g_pm.mode = PresetMenuMode::Naming;
    g_pm.target_id.clear();
    g_pm.target_name.clear();
    g_pm.status.clear();
    g_pm_name[0] = '\0';
    g_pm_focus_name = true;
}

void PmStartRenaming(const PresetInfo& p)
{
    if (p.factory) {
        PmStatus(kFactoryReadOnly);
        return;
    }
    g_pm.mode = PresetMenuMode::Renaming;
    g_pm.target_id = p.id;
    g_pm.target_name = p.name;
    g_pm.status.clear();
    std::snprintf(g_pm_name, sizeof(g_pm_name), "%s", p.name.c_str());
    g_pm_focus_name = true;
}

void PmConfirm(PresetConfirm kind, const std::string& id, const std::string& name, bool clash)
{
    if (kind == PresetConfirm::Overwrite && g_pm.mode != PresetMenuMode::Naming) g_pm_item = GetTaggingModel().item;
    g_pm.mode = PresetMenuMode::Confirm;
    g_pm.confirm = kind;
    g_pm.confirm_clash = clash;
    g_pm.target_id = id;
    g_pm.target_name = name;
    g_pm.status.clear();
}

void PmStartDelete(const PresetInfo& p)
{
    if (p.factory) {
        PmStatus(kFactoryReadOnly);
        return;
    }
    PmConfirm(PresetConfirm::Delete, p.id, p.name, false);
}

void PmCancel()
{
    g_pm.status = CancelStatus(g_pm.mode, g_pm.confirm);
    g_pm.mode = PresetMenuMode::List;
    g_pm.confirm = PresetConfirm::None;
    g_pm.confirm_clash = false;
    g_pm.target_id.clear();
    g_pm.target_name.clear();
}

void PmLoad(const std::string& id)
{
    Later([id]() { TaggingLoadPreset(id); });
    g_pm_close = true;
}

// Writes the item's rules to `id` (Save, or a Save as clash), then the item adopts it.
void PmOverwrite(const std::string& id, const std::string& name)
{
    Later([id, name]() {
        std::string saved, err;
        if (TaggingSavePreset(id, "", &saved, &err))
            PmStatus((saved.empty() ? name : saved) + " saved.");
        else
            PmStatus(err.empty() ? "The preset could not be saved." : err);
        PresetFilesChanged();
        g_pm.sel = id;
    });
}

void PmConfirmed()
{
    if (g_pm.confirm == PresetConfirm::Overwrite && !PmSameItem()) return;
    const PresetConfirm kind = g_pm.confirm;
    const std::string id = g_pm.target_id, name = g_pm.target_name;
    g_pm.mode = PresetMenuMode::List;
    g_pm.confirm = PresetConfirm::None;
    g_pm.confirm_clash = false;
    if (kind == PresetConfirm::Delete) {
        Later([id, name]() {
            std::string err;
            if (DeletePreset(RulesResourceRoot(), id, &err)) {
                PmStatus(name + " deleted.");
                if (g_pm.sel == id) g_pm.sel.clear();
            } else {
                PmStatus(err.empty() ? "The preset could not be deleted." : err);
            }
            PresetFilesChanged();
        });
    } else if (kind == PresetConfirm::Overwrite) {
        PmOverwrite(id, name);
    }
}

// Enter in the name field.
void PmSubmitName()
{
    const std::string name = TrimPresetName(g_pm_name);
    if (g_pm.mode == PresetMenuMode::Renaming) {
        const std::string id = g_pm.target_id, was = g_pm.target_name;
        g_pm.mode = PresetMenuMode::List;
        if (CheckRenameName(name) == NameCheck::Empty) {
            PmStatus(EmptyNameStatus(true));
            return;
        }
        Later([id, was, name]() {
            std::string err;
            if (RenamePreset(RulesResourceRoot(), id, name, &err))
                PmStatus(was + " renamed " + name + ".");
            else
                PmStatus(err.empty() ? "The preset could not be renamed." : err);
            PresetFilesChanged();
        });
        return;
    }
    // Save as.
    if (!PmSameItem()) return;
    PresetInfo clash;
    switch (CheckSaveAsName(CachedPresets(), name, &clash)) {
    case NameCheck::Empty:
        g_pm.mode = PresetMenuMode::List;
        PmStatus(EmptyNameStatus(false));
        return;
    case NameCheck::UserClash:
        PmConfirm(PresetConfirm::Overwrite, clash.id, clash.name, true);
        return;
    case NameCheck::Ok:
        break;
    }
    g_pm.mode = PresetMenuMode::List;
    Later([name]() {
        std::string saved, err;
        if (TaggingSavePreset("", name, &saved, &err)) {
            PmStatus((saved.empty() ? name : saved) + " saved to User.");
            g_pm.sel = GetTaggingModel().preset_id;
        } else {
            PmStatus(err.empty() ? "The preset could not be saved." : err);
        }
        PresetFilesChanged();
    });
}

void PmExport(const std::string& id, const std::string& name)
{
    g_dialog.kind = PendingDialog::Kind::Export;
    g_dialog.id = id;
    g_dialog.name = name;
    g_pm.status.clear();
}

void PmImport()
{
    g_dialog = PendingDialog{};
    g_dialog.kind = PendingDialog::Kind::Import;
    g_pm.status.clear();
}

// Text clipped to [x0, x1].
void ClippedText(ImDrawList* dl, float x0, float x1, float y, ImU32 col, const char* text)
{
    const ImVec2 lo(x0, y - 2.0f), hi(std::max(x0, x1), y + ImGui::GetTextLineHeight() + 2.0f);
    dl->PushClipRect(lo, hi, true);
    dl->AddText(ImVec2(x0, y), col, text);
    dl->PopClipRect();
}

// One preset row: the tick (the item's preset), the name, the padlock (Factory), the source tag
// while searching. Click selects, double-click loads, right-click opens its menu. `row_w` > 0
// fixes the width (a submenu sizes itself to its rows), else the row fills the window.
void PresetRow(const TaggingModel& m, const PresetInfo& p, bool with_source, float indent, float row_w = 0.0f)
{
    ImGui::PushID(p.id.c_str());
    const bool sel = g_pm.sel == p.id;
    const float rh = ImGui::GetFrameHeight();
    const ImVec2 r0 = ImGui::GetCursorScreenPos();
    const float rw = row_w > 0.0f ? row_w : ImGui::GetContentRegionAvail().x;
    if (ImGui::Selectable("##prow", sel, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_NoAutoClosePopups,
                          ImVec2(rw, rh))) {
        g_pm.sel = p.id;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) PmLoad(p.id);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float ty = r0.y + (rh - ImGui::GetTextLineHeight()) * 0.5f;
    float x = r0.x + indent;
    if (m.has_preset && m.preset_id == p.id) IconCheck(dl, ImVec2(x + 5.0f, r0.y + rh * 0.5f), 4.0f, ui::kAccent);
    x += 16.0f;
    const char* src = p.factory ? "Factory" : "User";
    const float src_w = with_source ? ImGui::CalcTextSize(src).x + 8.0f : 0.0f;
    const float lock_w = p.factory ? 14.0f : 0.0f;
    const float name_w =
        std::max(0.0f, std::min(ImGui::CalcTextSize(p.name.c_str()).x, r0.x + rw - src_w - lock_w - 6.0f - x));
    ClippedText(dl, x, x + name_w, ty, ui::kText, p.name.c_str());
    if (p.factory) IconLock(dl, ImVec2(x + name_w + 9.0f, r0.y + rh * 0.5f + 1.0f), 3.5f, ui::kMuted, true);
    if (with_source) dl->AddText(ImVec2(r0.x + rw - src_w + 2.0f, ty), ui::kFaint, src);
    if (ImGui::BeginPopupContextItem("##pctx")) {
        g_pm.sel = p.id;
        if (ImGui::MenuItem("Load")) PmLoad(p.id);
        if (p.factory) {
            ImGui::Separator();
            ImGui::TextDisabled("%s", kFactoryReadOnly);
        }
        if (ImGui::MenuItem("Rename...", "F2", false, !p.factory)) PmStartRenaming(p);
        if (ImGui::MenuItem("Delete", "Del", false, !p.factory)) PmStartDelete(p);
        if (ImGui::MenuItem("Export...")) PmExport(p.id, p.name);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

// A row's width in a cascade submenu (DM-XYZ-Pad's ROW_W).
constexpr float kPresetRowW = 260.0f;

// One cascade entry, as DM-XYZ-Pad: "Factory (n) >" opens a submenu on hover with its rows (or
// the empty text), then the folder they come from. True while its submenu is open.
bool PresetCascadeMenu(const TaggingModel& m, const std::vector<PresetInfo>& presets, bool factory)
{
    const std::string label = CascadeLabel(factory, CountPresets(presets, factory)) + (factory ? "##pcf" : "##pcu");
    if (!ImGui::BeginMenu(label.c_str())) return false;
    const std::vector<int> list = PresetMenuRows(presets, "", factory ? PresetCascade::Factory : PresetCascade::User);
    if (list.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kPresetRowW);
        ImGui::TextDisabled("%s", EmptyCascadeText(factory));
        ImGui::PopTextWrapPos();
    }
    for (int i : list) PresetRow(m, presets[static_cast<size_t>(i)], /*with_source=*/false, 4.0f, kPresetRowW);
    ImGui::Separator();
    if (factory) {
        ImGui::TextDisabled("Built into ReaAnimViewer");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Factory presets ship with the extension and update with it (read-only)");
    } else {
        // The folder, shortened from the start (its end is what tells folders apart).
        const std::string dir = UserPresetDir(RulesResourceRoot());
        std::string shown = dir;
        while (shown.size() > 4 && ImGui::CalcTextSize(shown.c_str()).x > kPresetRowW) {
            shown.erase(0, 4);
            while (!shown.empty() && (static_cast<unsigned char>(shown[0]) & 0xC0) == 0x80) shown.erase(0, 1);
        }
        if (shown != dir) shown = "..." + shown;
        ImGui::TextDisabled("%s", shown.c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", dir.c_str());
    }
    ImGui::EndMenu();
    return true;
}

bool AmberButton(const char* label)
{
    ImGui::PushStyleColor(ImGuiCol_Button, ui::Col(ui::kConfirmAct));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::Col(ui::kConfirmActHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui::Col(ui::kConfirmAct));
    ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::kConfirmActText));
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    return pressed;
}

void WrappedText(ImU32 col, const std::string& text, float wrap_w)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(col));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// The keys, read before the widgets (a mode change takes effect on the next frame's widgets).
void PresetMenuKeys(const std::vector<std::string>& visible)
{
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
    switch (g_pm.mode) {
    case PresetMenuMode::Confirm:
        if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) PmConfirmed();
        else if (pressed(ImGuiKey_Escape)) PmCancel();
        return;
    case PresetMenuMode::Naming:
    case PresetMenuMode::Renaming:
        if (pressed(ImGuiKey_Escape)) PmCancel();
        return;  // Enter: the name field's own
    case PresetMenuMode::List:
        break;
    }
    if (pressed(ImGuiKey_Escape)) {
        g_pm_close = true;
        return;
    }
    const bool down = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true), up = ImGui::IsKeyPressed(ImGuiKey_UpArrow, true);
    if (down || up) {
        if (visible.empty()) return;
        g_pm.sel = StepPresetSelection(visible, g_pm.sel, down ? 1 : -1);
        g_pm_arrowed = true;
        return;
    }
    // Only a row the list shows (not one filtered out, or under a closed cascade).
    if (std::find(visible.begin(), visible.end(), g_pm.sel) == visible.end()) return;
    const PresetInfo* sel = PresetById(g_pm.sel);
    if (!sel) return;
    if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) PmLoad(sel->id);
    else if (pressed(ImGuiKey_F2)) PmStartRenaming(*sel);
    // Del edits the search text while there is some; an empty (even focused) search lets it delete.
    else if (pressed(ImGuiKey_Delete) && (!g_pm_search_active || g_pm_query[0] == '\0')) PmStartDelete(*sel);
}

void DrawPresetMenu(const TaggingModel& m, float menu_w)
{
    const std::vector<PresetInfo>& presets = CachedPresets();
    const float inner_w = menu_w - ImGui::GetStyle().WindowPadding.x * 2.0f;
    const bool busy = g_pm.mode == PresetMenuMode::Confirm;

    // The rows the keys walk (last frame's query and submenu): the search hits, else the open
    // submenu, else the selection's folder, else Factory (DM-XYZ-Pad).
    PresetCascade walk = g_pm.cascade;
    if (walk == PresetCascade::None) {
        const PresetInfo* s0 = g_pm.sel.empty() ? nullptr : PresetById(g_pm.sel);
        walk = (s0 && !s0->factory) ? PresetCascade::User : PresetCascade::Factory;
    }
    const std::vector<int> rows = PresetMenuRows(presets, g_pm_query, walk);
    std::vector<std::string> visible;
    for (int i : rows) visible.push_back(presets[static_cast<size_t>(i)].id);
    if (!g_pm.sel.empty() && !PresetById(g_pm.sel)) g_pm.sel.clear();  // deleted meanwhile
    PresetMenuKeys(visible);
    if (g_pm_close) return;

    // Search.
    ImGui::BeginDisabled(busy);
    ImGui::SetNextItemWidth(-1.0f);
    if (g_pm_focus_search) {
        ImGui::SetKeyboardFocusHere();
        g_pm_focus_search = false;
    }
    const std::string ph = SearchPlaceholder(static_cast<int>(presets.size()));
    ImGui::InputTextWithHint("##pq", ph.c_str(), g_pm_query, sizeof(g_pm_query));
    g_pm_search_active = ImGui::IsItemActive();
    ImGui::Separator();

    // The list: the cascade, or one flat list while searching.
    if (g_pm_query[0] != '\0') {
        const std::vector<int> hits = PresetMenuRows(presets, g_pm_query, PresetCascade::None);
        if (hits.empty()) ImGui::TextDisabled("%s", kNoPresetMatches);
        for (int i : hits) PresetRow(m, presets[static_cast<size_t>(i)], /*with_source=*/true, 4.0f);
    } else {
        // Factory (n) > / User (n) >: submenus that open on hover.
        // Up / Down with no submenu open: open the walked folder's, so the selection shows.
        if (g_pm_arrowed && g_pm.cascade == PresetCascade::None) {
            const bool f = walk == PresetCascade::Factory;
            ImGui::OpenPopup((CascadeLabel(f, CountPresets(presets, f)) + (f ? "##pcf" : "##pcu")).c_str());
        }
        g_pm_arrowed = false;
        PresetCascade open = PresetCascade::None;
        if (PresetCascadeMenu(m, presets, /*factory=*/true)) open = PresetCascade::Factory;
        if (PresetCascadeMenu(m, presets, /*factory=*/false)) open = PresetCascade::User;
        g_pm.cascade = open;
    }
    ImGui::EndDisabled();
    ImGui::Separator();

    // The Save row, the name field, or the amber confirm.
    if (g_pm.mode == PresetMenuMode::Confirm) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        const ImVec2 b0 = ImGui::GetCursorScreenPos();
        ImGui::Indent(6.0f);
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        ImGui::TextUnformatted(ConfirmQuestion(g_pm.confirm, g_pm.target_name).c_str());
        WrappedText(ui::kMuted, ConfirmDetail(g_pm.confirm, g_pm.confirm_clash), inner_w - 12.0f);
        if (ui::SolidButton("Cancel##pccancel")) PmCancel();
        ImGui::SameLine();
        if (AmberButton(ConfirmAction(g_pm.confirm))) PmConfirmed();
        ImGui::TextDisabled("%s", ConfirmHint(g_pm.confirm));
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::Unindent(6.0f);
        const ImVec2 b1(b0.x + inner_w, ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y * 0.5f);
        dl->ChannelsSetCurrent(0);
        dl->AddRectFilled(b0, b1, WithAlpha(ui::kConfirmLine, 0x14), ui::kRadiusMd);
        dl->AddRect(b0, b1, ui::kConfirmLine, ui::kRadiusMd);
        dl->ChannelsMerge();
    } else if (g_pm.mode == PresetMenuMode::Naming || g_pm.mode == PresetMenuMode::Renaming) {
        if (g_pm.mode == PresetMenuMode::Renaming)
            ImGui::TextDisabled("Rename %s", g_pm.target_name.c_str());
        ImGui::SetNextItemWidth(-1.0f);
        if (g_pm_focus_name) {
            ImGui::SetKeyboardFocusHere();
            g_pm_focus_name = false;
        }
        if (ImGui::InputTextWithHint("##pname", "Preset name", g_pm_name, sizeof(g_pm_name),
                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
            PmSubmitName();
    } else {
        const bool can_write = m.has_record && !m.rules.blocks.empty();
        const bool factory = m.has_preset && m.preset_factory;
        const std::string save_label =
            std::string("Save") + (m.has_preset && !factory ? " " + m.preset_name : std::string()) + "##psave";
        const float as_w = ImGui::CalcTextSize("+ Save as...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const bool gone = m.has_preset && m.preset_gone;
        const bool unchanged = m.has_preset && !gone && m.preset_state == PresetState::UpToDate;
        ImGui::BeginDisabled(!can_write || factory || gone || unchanged);
        if (ui::SolidButton(save_label.c_str(), ImVec2(std::max(40.0f, inner_w - as_w - ImGui::GetStyle().ItemSpacing.x), 0.0f))) {
            if (!m.has_preset) PmStatus(kNothingLoadedToSave);
            else PmConfirm(PresetConfirm::Overwrite, m.preset_id, m.preset_name, false);
        }
        ImGui::EndDisabled();
        if (m.has_preset && !factory && can_write &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (gone)
                ImGui::SetTooltip("%s: Save as writes it again", kPresetGoneTip);
            else if (unchanged)
                ImGui::SetTooltip("No change to save: this item's rules are %s as it is", m.preset_name.c_str());
            else
                ImGui::SetTooltip("Overwrite %s with this item's rules", m.preset_name.c_str());
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!can_write);
        if (ui::SolidButton("+ Save as...##psaveas")) PmStartNaming();
        ImGui::EndDisabled();
        if (factory) WrappedText(ui::kFaint, FactorySaveNote(m.preset_name), inner_w);
        if (!can_write) WrappedText(ui::kFaint, "Nothing to save yet: add a rule (+ Rule) or load a preset.", inner_w);
    }

    // The status line (or the mode's hint).
    const std::string status = g_pm.status.empty() ? std::string(PresetMenuHint(g_pm.mode)) : g_pm.status;
    if (!status.empty()) WrappedText(ui::kMuted, status, inner_w);

    // The User folder, with Copy.
    ImGui::BeginDisabled(busy);
    {
        const std::string dir = UserPresetDir(RulesResourceRoot());
        const float fh = ImGui::GetFrameHeight();
        const ImVec2 r0 = ImGui::GetCursorScreenPos();
        const float path_w = std::max(20.0f, inner_w - fh - 6.0f);
        ImGui::Dummy(ImVec2(path_w, fh));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s\nYour User presets", dir.c_str());
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ClippedText(dl, r0.x, r0.x + path_w, r0.y + (fh - ImGui::GetTextLineHeight()) * 0.5f, ui::kFaint, dir.c_str());
        ImGui::SameLine();
        if (IconButton("##pcopy", fh, [](ImDrawList* d, ImVec2 c, ImU32 col) { IconDuplicate(d, c, 4.5f, col); }, !busy)) {
            ImGui::SetClipboardText(dir.c_str());
            PmStatus(kFolderCopied);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Copy the folder path");
    }
    // Import / Export.
    if (ImGui::SmallButton("Import...##pimport")) PmImport();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Copies a .ravpreset file into your User folder");
    ImGui::SameLine();
    if (ImGui::SmallButton("Export...##pexport")) {
        const PresetInfo* p = !g_pm.sel.empty() ? PresetById(g_pm.sel) : nullptr;
        if (!p && m.has_preset && !m.preset_gone) p = PresetById(m.preset_id);
        if (p) PmExport(p->id, p->name);
        else PmStatus("Select a preset to export.");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Writes the selected (or loaded) preset to a file you can send");
    ImGui::EndDisabled();
}

// The preset field: Preset: [padlock] Footsteps  edited / Legacy / kept v2, and its menu.
// Under it, the Legacy band (Update / Keep) when the preset on disk has another version.
void DrawPresetField(const TaggingModel& m)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fh = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::InvisibleButton("##presetfield", ImVec2(w, fh))) {
        CachedPresets(/*refresh=*/true);  // the menu opens on the files as they are now
        ResetPresetMenu();
        g_pm.open = true;
        g_pm_focus_search = true;
        ImGui::OpenPopup("##tagpresets");
    }
    const bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + fh), hov ? ui::kHover : ui::kRaised, ui::kRadiusSm);
    dl->AddRect(p, ImVec2(p.x + w, p.y + fh), g_pm.open ? ui::kAccentLine : ui::kStroke, ui::kRadiusSm);
    const float ty = p.y + (fh - ImGui::GetTextLineHeight()) * 0.5f;
    float x = p.x + 8.0f;
    dl->AddText(ImVec2(x, ty), ui::kMuted, "Preset:");
    x += ImGui::CalcTextSize("Preset:").x + 6.0f;
    if (m.has_preset && m.preset_factory) {
        IconLock(dl, ImVec2(x + 4.0f, p.y + fh * 0.5f + 1.0f), 4.0f, ui::kMuted, true);
        x += 12.0f;
    }
    const char* name = m.has_preset ? m.preset_name.c_str() : "none";
    const bool legacy = m.has_preset && m.preset_state == PresetState::Legacy;
    const bool kept = ShowKeptTag(m.has_preset, m.preset_gone, m.kept_version, m.disk_version);
    std::string tag;
    if (legacy) {
        tag = "Legacy";
    } else if (m.has_preset) {
        if (m.preset_state == PresetState::Edited) tag = "edited";
        if (kept) tag += (tag.empty() ? "" : "  ") + KeptTag(m.preset_version);
    }
    const float tag_w = !tag.empty() ? ImGui::CalcTextSize(tag.c_str()).x + 12.0f : 0.0f;
    ClippedText(dl, x, p.x + w - tag_w - 18.0f, ty, m.preset_gone ? ui::kMuted : ui::kText, name);
    if (!tag.empty()) dl->AddText(ImVec2(p.x + w - tag_w - 14.0f, ty), legacy ? ui::kWarn : ui::kFaint, tag.c_str());
    // The chevron.
    const ImVec2 cc(p.x + w - 10.0f, p.y + fh * 0.5f);
    dl->AddTriangleFilled(ImVec2(cc.x - 3.5f, cc.y - 2.0f), ImVec2(cc.x + 3.5f, cc.y - 2.0f), ImVec2(cc.x, cc.y + 2.5f),
                          ui::kMuted);
    if (hov && !g_pm.open && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        if (m.preset_gone)
            ImGui::SetTooltip("%s", kPresetGoneTip);
        else
            ImGui::SetTooltip(m.preset_factory ? "A factory preset (read-only). Click for the preset menu."
                                               : "Click for the preset menu: load, save, rename, import, export");
    }

    // The menu.
    ImGui::SetNextWindowPos(ImVec2(p.x, p.y + fh + 2.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.0f), ImVec2(w, 560.0f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ui::Col(ui::kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, ui::Col(ui::kStrokeStrong));
    g_pm_drawn_frame = ImGui::GetFrameCount();
    const bool open = ImGui::BeginPopup("##tagpresets");
    ImGui::PopStyleColor(2);
    if (open) {
        if (!g_pm.open) {  // opened another way (never): start clean
            ResetPresetMenu();
            g_pm.open = true;
        }
        DrawPresetMenu(m, w);
        if (g_pm_close) {
            ImGui::CloseCurrentPopup();
            ResetPresetMenu();
        }
        ImGui::EndPopup();
    } else if (g_pm.open) {
        ResetPresetMenu();  // closed (a click outside, Esc): a pending confirm or name counts as Cancel
    }

    // The Legacy band: "Legacy - v3 is installed  [Update] [Keep v2]" (the buttons wrap under
    // the text when the panel is narrow).
    if (m.has_record && m.has_preset && !m.preset_gone && m.preset_state == PresetState::Legacy) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        const ImGuiStyle& st = ImGui::GetStyle();
        const std::string text = LegacyText(m.disk_version);
        const std::string keep = KeepLabel(m.preset_version);
        const float keep_w = ImGui::CalcTextSize(keep.c_str()).x + st.FramePadding.x * 2.0f;
        const float up_w = ImGui::CalcTextSize("Update").x + st.FramePadding.x * 2.0f;
        const float text_w = ImGui::CalcTextSize(text.c_str()).x;
        const bool one_line = 8.0f + text_w + 12.0f + up_w + st.ItemSpacing.x + keep_w + 6.0f <= w;
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        const ImVec2 b0 = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(b0.x + 8.0f, b0.y + 3.0f));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ui::Col(ui::kWarn), "%s", text.c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("This item uses v%d of %s. Update takes the installed version; Keep stays on v%d.",
                              m.preset_version, m.preset_name.c_str(), m.preset_version);
        const float bx = b0.x + w - 6.0f - keep_w - up_w - st.ItemSpacing.x;
        if (one_line) {
            ImGui::SameLine();
            ImGui::SetCursorScreenPos(ImVec2(std::max(ImGui::GetCursorScreenPos().x, bx), ImGui::GetCursorScreenPos().y));
        } else {
            ImGui::SetCursorScreenPos(ImVec2(std::max(b0.x + 8.0f, bx), ImGui::GetCursorScreenPos().y));
        }
        if (AmberButton("Update##lgup")) {
            Later([]() { TaggingUpdatePreset(); });  // the footer's last Commit line goes stale with the write
        }
        ImGui::SameLine();
        if (ui::SolidButton((keep + "##lgkeep").c_str())) Later([]() { TaggingKeepPreset(); });
        const float y1 = ImGui::GetItemRectMax().y + 3.0f;
        dl->ChannelsSetCurrent(0);
        dl->AddRectFilled(b0, ImVec2(b0.x + w, y1), WithAlpha(ui::kWarn, 0x14), ui::kRadiusSm);
        dl->AddRect(b0, ImVec2(b0.x + w, y1), ui::kConfirmLine, ui::kRadiusSm);
        dl->ChannelsMerge();
        ImGui::SetCursorScreenPos(ImVec2(b0.x, y1));
        ImGui::Dummy(ImVec2(w, 0.0f));
    }
}

}  // namespace

bool TaggingPresetMenuOpen()
{
    // Only while it is drawn: the field goes when the item does (the popup is then gone too).
    if (!g_pm.open || !ImGui::GetCurrentContext()) return false;
    const int age = ImGui::GetFrameCount() - g_pm_drawn_frame;  // negative: another ImGui context since
    return age >= 0 && age <= 1;
}

bool TaggingHasPendingDialog()
{
    return g_dialog.kind != PendingDialog::Kind::None;
}

void TaggingRunPendingDialog(HWND__* owner)
{
    const PendingDialog d = g_dialog;  // taken first: the picker's modal loop runs frames
    g_dialog = PendingDialog{};
    const std::string root = RulesResourceRoot();
    if (d.kind == PendingDialog::Kind::Import) {
        std::string path;
        if (!PickOpenFile(owner, L"Import a preset", kPresetFilter, path)) return;  // cancelled
        std::string id, err;
        if (ImportPreset(root, path, &id, &err)) {
            PresetInfo info;
            const std::string name = FindPreset(root, id, &info) ? info.name : id;
            PmStatus(name + " imported.");
            g_pm.sel = id;
        } else {
            PmStatus(err.empty() ? "The file could not be imported." : err);
        }
        PresetFilesChanged();
    } else if (d.kind == PendingDialog::Kind::Export) {
        std::string path;
        if (!PickSaveFile(owner, L"Export a preset", kPresetFilter, ExportFileName(d.name), path)) return;
        std::string err;
        if (ExportPreset(root, d.id, path, &err))
            PmStatus(d.name + " exported.");
        else
            PmStatus(err.empty() ? "The preset could not be exported." : err);
    } else if (d.kind == PendingDialog::Kind::RolesImport || d.kind == PendingDialog::Kind::RolesExport) {
        RolesRunDialog(owner, d);
    }
}

namespace {

// ---- Story 10-3d: the Skeleton & roles window -------------------------------------------------
//
// Opened from Roles: one row per role, the bone that plays it, and where it comes from
// (auto = the guess, you = roles.txt, none). A choice is written at once to roles.txt (per
// skeleton, for every project: role_map_store.h), never to the project, so it is no REAPER
// undo point: the window keeps its own undo (Ctrl+Z, the undo button) while it is open.
//
// Story 10-3e: the rows are role keys: the 9 built-in roles, then the user's own roles (created
// with + Role, renamed in place, deleted with an amber confirm), then the role keys the item's
// rules read that are in no list ("not in your roles", still mappable). Create, rename and
// delete are steps of the same undo.

constexpr char  kRolesId[] = "##rolesdlg";
constexpr float kRolesW = 440.0f;

enum class RoleRowKind { Builtin, Custom, Foreign };
struct RoleRow {
    std::string key;
    RoleRowKind kind = RoleRowKind::Builtin;
    std::string name;  // the display name ("left heel", "sword tip", "tail end")
};

enum class RoleUndoKind { Bone, Created, Renamed, Deleted, Imported };
struct RoleUndoStep {
    RoleUndoKind    kind = RoleUndoKind::Bone;
    std::string     key;
    StoredRoleEntry prev;       // Bone: the entry the change replaced, exactly as stored
    std::string     name;       // Renamed: the old name; Deleted: its name
    int             index = -1; // Deleted: its place in the list
    RoleMapSnapshot text;       // Imported: roles.txt as it was before the import
};

bool                                   g_roles_request = false;  // the Roles button asked for the window
bool                                   g_roles_open = false;
int                                    g_roles_drawn_frame = -10;
MediaItem*                             g_roles_item = nullptr;   // the item it was opened on
std::vector<std::string>               g_roles_names;            // ...its skeleton: the writes' key
std::vector<int>                       g_roles_depth;            // per bone: its depth (the menu's indent)
std::vector<std::string>               g_roles_read_keys;        // the row keys the entries below were read for
std::map<std::string, StoredRoleEntry> g_roles_entries;          // roles.txt's entries, read after each change
std::map<std::string, int>             g_roles_guess;            // the guess per key (the "Auto (...)" entry)
std::vector<RoleUndoStep>              g_roles_undo;
std::string                            g_roles_err;              // the last write's refusal
std::string                            g_roles_status;           // 10-3f: the last Import / Export result
char                                   g_roles_query[64] = {};
bool                                   g_roles_menu_appearing = false;  // a bone menu opened this frame
char                                   g_roles_new[64] = {};     // the + Role field
std::string                            g_roles_rename_key;       // the row renamed in place ("" = none)
char                                   g_roles_rename_buf[64] = {};
bool                                   g_roles_rename_focus = false;
std::string                            g_roles_confirm_key;      // the row whose delete is being confirmed
std::string                            g_roles_hovered_key;      // the custom row hovered last frame (F2)
std::string                            g_roles_last_custom;      // the custom row clicked last (F2)

// The rows: built-in, the user's roles, then the keys the rules read that are in no list.
std::vector<RoleRow> RolesRows(const TaggingModel& m)
{
    std::vector<RoleRow> rows;
    for (int r = 0; r < static_cast<int>(Role::Count); ++r)
        rows.push_back({RoleKey(static_cast<Role>(r)), RoleRowKind::Builtin, RoleName(static_cast<Role>(r))});
    if (m.roles_status == RoleMapStatus::Unreadable) return rows;  // no write can go through: the 9 only
    for (const CustomRole& c : m.custom_roles) rows.push_back({c.key, RoleRowKind::Custom, c.name});
    for (const std::string& k : m.rule_role_keys) {
        bool listed = false;
        for (const CustomRole& c : m.custom_roles) listed = listed || c.key == k;
        if (!listed) rows.push_back({k, RoleRowKind::Foreign, RoleNameFromKey(k)});
    }
    return rows;
}

std::vector<std::string> RowKeys(const std::vector<RoleRow>& rows)
{
    std::vector<std::string> keys;
    for (const RoleRow& r : rows) keys.push_back(r.key);
    return keys;
}

// The entries and guesses of these keys, as roles.txt is now.
void RolesReadEntries(const std::vector<std::string>& keys)
{
    const std::string                  root = RulesResourceRoot();
    const std::vector<StoredRoleEntry> e = GetStoredRoles(root, g_roles_names, keys);
    const RoleMapFile                  f = ReadRoleMapFile(root);
    g_roles_entries.clear();
    g_roles_guess.clear();
    for (size_t i = 0; i < keys.size(); ++i) {
        g_roles_entries[keys[i]] = i < e.size() ? e[i] : StoredRoleEntry{};
        g_roles_guess[keys[i]] = GuessRoleKey(f, g_roles_names, keys[i]);
    }
    g_roles_read_keys = keys;
}

// After a write: the model (bone column, rows, count, strip) first, then the entries of its rows.
void RolesRefresh()
{
    g_roles_status.clear();  // an Import / Export result sets it again after its refresh
    TaggingReread();
    if (g_roles_open) RolesReadEntries(RowKeys(RolesRows(GetTaggingModel())));
}

void RolesForget()
{
    g_roles_open = false;
    g_roles_item = nullptr;
    g_roles_names.clear();
    g_roles_depth.clear();
    g_roles_read_keys.clear();
    g_roles_entries.clear();
    g_roles_guess.clear();
    g_roles_undo.clear();  // the window's undo history goes with it
    g_roles_err.clear();
    g_roles_status.clear();
    g_roles_new[0] = '\0';
    g_roles_rename_key.clear();
    g_roles_confirm_key.clear();
    g_roles_hovered_key.clear();
    g_roles_last_custom.clear();
}

bool RolesSameSkeleton(const std::vector<std::string>& names)
{
    return g_roles_open && names == g_roles_names;
}

void RolesOpen(const TaggingModel& m)
{
    RolesForget();
    g_roles_item = m.item;
    g_roles_names = m.bone_names;
    const int n = static_cast<int>(m.bone_names.size());
    g_roles_depth.assign(static_cast<size_t>(n), 0);
    for (int i = 0; i < n && i < static_cast<int>(m.bone_parents.size()); ++i) {
        int d = 0;
        for (int p = m.bone_parents[static_cast<size_t>(i)]; p >= 0 && p < n && d < n; ++d)
            p = p < static_cast<int>(m.bone_parents.size()) ? m.bone_parents[static_cast<size_t>(p)] : -1;
        g_roles_depth[static_cast<size_t>(i)] = d;
    }
    g_roles_open = true;
    RolesReadEntries(RowKeys(RolesRows(m)));
    Later([]() { RolesRefresh(); });  // the bone column and the count follow roles.txt as it is now
    g_roles_drawn_frame = ImGui::GetFrameCount();
    ImGui::OpenPopup(kRolesId);
}

void RolesFailed(const std::string& err, const char* fallback)
{
    g_roles_err = err.empty() ? std::string(fallback) : err;
    g_roles_status.clear();
}

// One role change, written after the frame (ApplyRoleChange). The entry it replaced goes on the
// window's undo stack when the entry changed; a refusal shows in red.
void RolesChange(const std::string& key, RoleChangeKind kind, const std::string& bone)
{
    const std::vector<std::string> names = g_roles_names;
    Later([key, kind, bone, names]() {
        StoredRoleEntry prev;
        bool            changed = false;
        std::string     err;
        const bool ok = ApplyRoleChange(RulesResourceRoot(), names, key, kind, bone, &prev, &changed, &err);
        if (RolesSameSkeleton(names)) {
            if (ok) {
                if (changed) g_roles_undo.push_back({RoleUndoKind::Bone, key, prev, "", -1, RoleMapSnapshot{}});
                g_roles_err.clear();
            } else {
                RolesFailed(err, "The role mapping could not be saved.");
            }
        }
        RolesRefresh();  // the strip, Roles and the footer follow at once
    });
}

// Story 10-3e -- + Role: a new role from the typed name.
void RolesCreate(const std::string& name)
{
    const std::vector<std::string> names = g_roles_names;
    Later([name, names]() {
        std::string key, err;
        const bool  ok = AddCustomRole(RulesResourceRoot(), name, &key, &err);
        if (RolesSameSkeleton(names)) {
            if (ok) {
                g_roles_undo.push_back({RoleUndoKind::Created, key, StoredRoleEntry{}, "", -1, RoleMapSnapshot{}});
                g_roles_err.clear();
                g_roles_new[0] = '\0';
                g_roles_last_custom = key;
            } else {
                RolesFailed(err, "The role could not be saved.");
            }
        }
        RolesRefresh();
    });
}

void RolesRename(const std::string& key, const std::string& name)
{
    const std::vector<std::string> names = g_roles_names;
    Later([key, name, names]() {
        std::string old_name, err;
        const bool  ok = RenameCustomRole(RulesResourceRoot(), key, name, &old_name, &err);
        if (RolesSameSkeleton(names)) {
            if (ok) {
                // A rename to the same name writes nothing: no undo step.
                bool same = false;
                for (const CustomRole& c : GetCustomRoles(RulesResourceRoot()))
                    if (c.key == key) same = c.name == old_name;
                if (!same)
                    g_roles_undo.push_back({RoleUndoKind::Renamed, key, StoredRoleEntry{}, old_name, -1, RoleMapSnapshot{}});
                g_roles_err.clear();
            } else {
                RolesFailed(err, "The role could not be renamed.");
            }
        }
        RolesRefresh();
    });
}

void RolesDelete(const std::string& key)
{
    const std::vector<std::string> names = g_roles_names;
    Later([key, names]() {
        std::string name, err;
        int         index = -1;
        const bool  ok = DeleteCustomRole(RulesResourceRoot(), key, &name, &index, &err);
        if (RolesSameSkeleton(names)) {
            if (ok) {
                if (index >= 0)
                    g_roles_undo.push_back({RoleUndoKind::Deleted, key, StoredRoleEntry{}, name, index, RoleMapSnapshot{}});
                g_roles_err.clear();
                if (g_roles_last_custom == key) g_roles_last_custom.clear();
            } else {
                RolesFailed(err, "The role could not be deleted.");
            }
        }
        RolesRefresh();
    });
}

// The window's undo: the last step put back (an entry as it was stored, a role's name, a
// deleted role, a created one removed). Refused: the step stays.
void RolesUndo()
{
    if (g_roles_undo.empty()) return;
    const std::vector<std::string> names = g_roles_names;
    Later([names]() {
        if (!RolesSameSkeleton(names) || g_roles_undo.empty()) return;
        const RoleUndoStep s = g_roles_undo.back();
        const std::string  root = RulesResourceRoot();
        std::string        err;
        bool               ok = false;
        switch (s.kind) {
        case RoleUndoKind::Bone: ok = RestoreRole(root, names, s.key, s.prev, &err); break;
        case RoleUndoKind::Created: ok = DeleteCustomRole(root, s.key, nullptr, nullptr, &err); break;
        case RoleUndoKind::Renamed: ok = RestoreCustomRole(root, s.key, s.name, -1, &err); break;
        case RoleUndoKind::Deleted: ok = RestoreCustomRole(root, s.key, s.name, s.index, &err); break;
        case RoleUndoKind::Imported: ok = RestoreRoleMapText(root, s.text, &err); break;
        }
        if (ok) {
            g_roles_undo.pop_back();
            g_roles_err.clear();
        } else {
            RolesFailed(err, "The role mapping could not be saved.");
        }
        RolesRefresh();
    });
}

// ---- Story 10-3f: Import / Export of the role config (a .csv file) ----------------------------
//
// The pickers run from the window procedure (TaggingRunPendingDialog), never inside the frame.
// Export writes every role and this skeleton's choices; Import merges a file into roles.txt as
// one step of the window's undo (roles.txt put back byte-identical).

const FileDialogFilter kRolesCsvFilter = {L"CSV (*.csv)", L"*.csv", L"csv"};
// The proposed name: RAV knows no rig name (a skeleton is its bone list), so the user types it
// after the prefix (Antho, 2026-10-06); the picker adds .csv.
constexpr char         kRolesExportName[] = "RAV_Roles_";
constexpr char         kPrefRolesCsvSemicolon[] = "roles_csv_semicolon";  // the last export's separator

void RolesRequestDialog(PendingDialog::Kind kind, const TaggingModel& m)
{
    g_dialog = PendingDialog{};
    g_dialog.kind = kind;
    g_dialog.bones = g_roles_names;
    g_dialog.keys = m.rule_role_keys;
    g_roles_status.clear();
}

void RolesRunDialog(HWND__* owner, const PendingDialog& d)
{
    if (!RolesSameSkeleton(d.bones)) return;  // the window closed (or moved to another rig) meanwhile
    const std::string root = RulesResourceRoot();
    std::string       path, err;
    if (d.kind == PendingDialog::Kind::RolesExport) {
        const FileDialogFilter filters[] = {
            {L"CSV, comma separated (*.csv)", L"*.csv", L"csv"},
            {L"CSV, semicolon separated (Excel in French and other regions) (*.csv)", L"*.csv", L"csv"},
        };
        int type = LoadPrefBool(kPrefRolesCsvSemicolon, false) ? 1 : 0;
        if (!PickSaveFile(owner, L"Export roles", filters, 2, &type, kRolesExportName, path)) return;  // cancelled
        SavePrefBool(kPrefRolesCsvSemicolon, type == 1);
        const bool ok = ExportRoleConfig(root, d.bones, d.keys, type == 1 ? ';' : ',', path, &err);
        if (!RolesSameSkeleton(d.bones)) return;
        if (ok) {
            g_roles_err.clear();
            std::string file = path;
            const size_t slash = file.find_last_of("\\/");
            if (slash != std::string::npos) file.erase(0, slash + 1);
            g_roles_status = "Roles exported to " + file + ".";
        } else {
            RolesFailed(err, "The roles could not be exported.");
        }
        return;
    }
    if (!PickOpenFile(owner, L"Import roles", kRolesCsvFilter, path)) return;  // cancelled
    if (!RolesSameSkeleton(d.bones)) return;
    RoleImportReport rep;
    RoleMapSnapshot  prev;
    const bool       ok = ImportRoleConfig(root, d.bones, path, &rep, &prev, &err);
    if (ok && rep.changed()) g_roles_undo.push_back({RoleUndoKind::Imported, "", StoredRoleEntry{}, "", -1, prev});
    if (ok) g_roles_err.clear();
    RolesRefresh();  // the strip, Roles and the footer follow at once
    if (!RolesSameSkeleton(d.bones)) return;
    if (ok)
        g_roles_status = RoleImportSummary(rep);
    else
        RolesFailed(err, "The file could not be imported.");
}

void RolesStartRename(const std::string& key, const std::string& name)
{
    g_roles_confirm_key.clear();
    g_roles_rename_key = key;
    std::snprintf(g_roles_rename_buf, sizeof(g_roles_rename_buf), "%s", name.c_str());
    g_roles_rename_focus = true;
    g_roles_last_custom = key;
}

void IconUndo(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
    const float pi = 3.14159265f;
    dl->PathArcTo(ImVec2(c.x, c.y + 1.5f), r, pi, pi * 2.45f, 14);
    dl->PathStroke(col, ImDrawFlags_None, 1.5f);
    const ImVec2 s(c.x - r, c.y + 1.5f);
    dl->AddTriangleFilled(ImVec2(s.x - 3.2f, s.y - 1.0f), ImVec2(s.x + 3.2f, s.y - 1.0f), ImVec2(s.x, s.y + 3.5f), col);
}

// The mock-up's .tag pill, vertically centred on a frame-high line.
void TagPill(const char* text, ImU32 text_col, ImU32 edge)
{
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  fh = ImGui::GetFrameHeight();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float  h = ts.y + 4.0f, w = ts.x + 18.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, fh));
    const float y = p.y + (fh - h) * 0.5f;
    dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w, y + h), ui::kBg, h * 0.5f);
    dl->AddRect(ImVec2(p.x, y), ImVec2(p.x + w, y + h), edge, h * 0.5f);
    dl->AddText(ImVec2(p.x + 9.0f, y + 2.0f), text_col, text);
}

bool ContainsNoCase(const std::string& hay, const char* needle)
{
    const size_t n = std::strlen(needle);
    if (n == 0) return true;
    auto low = [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); };
    for (size_t i = 0; i + n <= hay.size(); ++i) {
        size_t k = 0;
        while (k < n && low(hay[i + k]) == low(needle[k])) ++k;
        if (k == n) return true;
    }
    return false;
}

// A row's menu: Auto (the guess), no bone, then the bones (searchable, indented by depth).
void RolesMenu(const std::string& key, const StoredRoleEntry& e)
{
    if (ImGui::IsWindowAppearing()) {
        g_roles_query[0] = '\0';
        g_roles_menu_appearing = true;
    }
    const auto        git = g_roles_guess.find(key);
    const int         guess = git != g_roles_guess.end() ? git->second : -1;
    const std::string auto_label =
        std::string("Auto (") +
        ((guess >= 0 && guess < static_cast<int>(g_roles_names.size())) ? g_roles_names[static_cast<size_t>(guess)]
                                                                        : std::string("no guess")) +
        ")";
    const bool builtin = RoleFromKey(key, nullptr);
    if (ImGui::Selectable(auto_label.c_str(), !e.stored) && e.stored) RolesChange(key, RoleChangeKind::Auto, "");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip(builtin ? "Back to RAV's guess from the bone names"
                                  : "Back to RAV's guess: a bone named like the one you picked for this role on "
                                    "another skeleton");
    const bool is_none = e.stored && e.bone.empty();
    if (ImGui::Selectable("\xE2\x80\x94 none \xE2\x80\x94", is_none) && !is_none) RolesChange(key, RoleChangeKind::None, "");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("No bone plays this role");
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1.0f);
    if (g_roles_menu_appearing) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##rq", "Search bones", g_roles_query, sizeof(g_roles_query));
    const float lh = ImGui::GetTextLineHeightWithSpacing();
    const float list_h = std::min(300.0f, lh * static_cast<float>(std::max<size_t>(1, g_roles_names.size())) + 8.0f);
    if (ImGui::BeginChild("##rbones", ImVec2(0.0f, list_h), ImGuiChildFlags_None)) {
        const bool searching = g_roles_query[0] != '\0';
        int shown = 0;
        for (size_t i = 0; i < g_roles_names.size(); ++i) {
            const std::string& nm = g_roles_names[i];
            if (searching && !ContainsNoCase(nm, g_roles_query)) continue;
            ++shown;
            ImGui::PushID(static_cast<int>(i));
            if (!searching && i < g_roles_depth.size())
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0f * static_cast<float>(std::min(g_roles_depth[i], 20)));
            // The store also matches a stored bone by its normalized name (another namespace).
            const bool sel = e.stored && !e.bone.empty() &&
                             (nm == e.bone || NormalizeBoneName(nm) == NormalizeBoneName(e.bone));
            if (ImGui::Selectable(nm.c_str(), sel)) {
                if (!sel) RolesChange(key, RoleChangeKind::Bone, nm);
                ImGui::CloseCurrentPopup();  // the list is a child: Selectable alone keeps the menu open
            }
            if (sel && g_roles_menu_appearing) ImGui::SetScrollHereY(0.5f);
            ImGui::PopID();
        }
        if (shown == 0) ImGui::TextDisabled("No bone matches");
    }
    ImGui::EndChild();
    g_roles_menu_appearing = false;
}

// The role's name as a row label: "Left heel", "Sword tip".
std::string RoleRowLabel(const std::string& name)
{
    std::string s = name;
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

// The amber in-place confirm of a role's delete (the preset menu's pattern).
void RolesDeleteConfirm(const RoleRow& row, bool used, float inner_w)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    const ImVec2 b0 = ImGui::GetCursorScreenPos();
    ImGui::Indent(6.0f);
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    ImGui::TextUnformatted(("Delete the role \"" + row.name + "\"?").c_str());
    WrappedText(ui::kMuted,
                std::string(used ? "These rules use it. " : "") + "Rules and presets keep it; its bones stay remembered.",
                inner_w - 12.0f);
    if (ui::SolidButton("Cancel##rdcancel")) g_roles_confirm_key.clear();
    ImGui::SameLine();
    if (AmberButton("Delete##rdok")) {
        RolesDelete(row.key);
        g_roles_confirm_key.clear();
    }
    ImGui::Dummy(ImVec2(0.0f, 1.0f));
    ImGui::Unindent(6.0f);
    const ImVec2 b1(b0.x + inner_w, ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y * 0.5f);
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(b0, b1, WithAlpha(ui::kConfirmLine, 0x14), ui::kRadiusMd);
    dl->AddRect(b0, b1, ui::kConfirmLine, ui::kRadiusMd);
    dl->ChannelsMerge();
}

void RolesWindow(const TaggingModel& m)
{
    if (g_roles_request) {
        g_roles_request = false;
        if (m.item && m.file_loaded && !m.bone_names.empty()) RolesOpen(m);
    }
    if (!g_roles_open) return;
    const int frame = ImGui::GetFrameCount();
    const int age = frame - g_roles_drawn_frame;
    // The item (or its skeleton) changed, or the window was not drawn (another view): it closes.
    bool close = age < 0 || age > 1 || m.item != g_roles_item || m.bone_names != g_roles_names;

    // The rows as the model has them now; their entries are read again when the set changed
    // (a role created, deleted, or read by the rules).
    const std::vector<RoleRow> rows = RolesRows(m);
    if (!close && RowKeys(rows) != g_roles_read_keys) RolesReadEntries(RowKeys(rows));
    if (!g_roles_rename_key.empty() || !g_roles_confirm_key.empty()) {
        bool rename_ok = g_roles_rename_key.empty(), confirm_ok = g_roles_confirm_key.empty();
        for (const RoleRow& r : rows) {
            if (r.kind != RoleRowKind::Custom) continue;
            rename_ok = rename_ok || r.key == g_roles_rename_key;
            confirm_ok = confirm_ok || r.key == g_roles_confirm_key;
        }
        if (!rename_ok) g_roles_rename_key.clear();
        if (!confirm_ok) g_roles_confirm_key.clear();
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::max(260.0f, std::min(kRolesW, vp->Size.x - 32.0f)), 0.0f), ImGuiCond_Always);
    // A short docked panel: the body scrolls instead of running off-screen.
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, std::max(120.0f, vp->Size.y - 32.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ui::Col(ui::kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, ui::Col(ui::kStrokeStrong));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::BeginPopupModal(kRolesId, nullptr, kFlags)) {
        g_roles_drawn_frame = frame;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float fh = ImGui::GetFrameHeight();
        const bool  writable = m.roles_status != RoleMapStatus::Unreadable;
        // Esc cancels a rename or a delete confirm, else closes; Ctrl+Z undoes the last step;
        // F2 renames the hovered (else the last clicked) custom role (the viewer routes every key
        // here while the window is open: TaggingRolesWindowOpen). A bone menu open has them first.
        if (!close && ImGui::IsWindowFocused()) {
            const ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                if (!g_roles_rename_key.empty()) g_roles_rename_key.clear();
                else if (!g_roles_confirm_key.empty()) g_roles_confirm_key.clear();
                else if (!io.WantTextInput) close = true;  // in the + Role field, Esc only leaves it
            } else if (ImGui::IsKeyPressed(ImGuiKey_Z, false) && io.KeyCtrl && !io.KeyShift && !io.KeyAlt &&
                       !io.WantTextInput) {
                RolesUndo();
            } else if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && !io.WantTextInput && writable) {
                const std::string& target = !g_roles_hovered_key.empty() ? g_roles_hovered_key : g_roles_last_custom;
                for (const RoleRow& r : rows)
                    if (r.kind == RoleRowKind::Custom && r.key == target) RolesStartRename(r.key, r.name);
            }
        }
        g_roles_hovered_key.clear();

        // Title: the name, found / not found (every row), undo, close.
        int not_found = 0;
        for (const RoleRow& r : rows) {
            Role role;
            int  b = -1;
            if (RoleFromKey(r.key, &role)) {
                const size_t i = static_cast<size_t>(role);
                b = i < m.role_to_bone.size() ? m.role_to_bone[i] : -1;
            } else {
                const auto it = m.custom_role_to_bone.find(r.key);
                b = it != m.custom_role_to_bone.end() ? it->second : -1;
            }
            if (b < 0 || b >= static_cast<int>(m.bone_names.size())) ++not_found;
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Skeleton & roles");
        ImGui::SameLine(0.0f, 8.0f);
        char count[32];
        if (not_found > 0) {
            std::snprintf(count, sizeof(count), "%d not found", not_found);
            TagPill(count, kBadText, WithAlpha(kBad, 0x80));
        } else {
            TagPill("all found", ui::kMuted, ui::kStroke);
        }
        // The explanation sits behind "?" (the window stays short; the README has the rest).
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 3.0f * fh - 8.0f);
        IconButton("##rhelp", fh, [](ImDrawList* d, ImVec2 c, ImU32 col) {
            const ImVec2 ts = ImGui::CalcTextSize("?");
            d->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, "?");
        });
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
            ImGui::TextUnformatted("Presets speak in roles (heel, toe, knee\xE2\x80\xA6). RAV guesses the bones from "
                                   "their names and remembers your choice for every item with this skeleton.");
            ImGui::TextUnformatted("+ Role adds your own roles, for every project. Right-click or F2 renames one.");
            ImGui::TextUnformatted("Role changes are saved at once. Ctrl+Z here undoes them while this window is open.");
            ImGui::TextUnformatted("Export... writes your roles and this skeleton's bones to a .csv file (a spreadsheet "
                                   "or a text editor opens it); Import... reads one into your roles for this skeleton.");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        ImGui::SameLine(0.0f, 4.0f);
        if (IconButton("##rundo", fh, [](ImDrawList* d, ImVec2 c, ImU32 col) { IconUndo(d, c, 5.0f, col); },
                       !g_roles_undo.empty()))
            RolesUndo();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Undo the last change (Ctrl+Z)");
        ImGui::SameLine(0.0f, 4.0f);
        if (IconButton("##rclose", fh, [](ImDrawList* d, ImVec2 c, ImU32 col) { IconCross(d, c, 4.0f, col); }))
            close = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Close (Esc)");

        char sub[64];
        std::snprintf(sub, sizeof(sub), "Skeleton: %d bones", static_cast<int>(g_roles_names.size()));
        ui::SubText(sub);
        ImGui::Dummy(ImVec2(0.0f, 2.0f));

        // One row per role: the role, its bone menu, where the bone comes from, ✕ (custom roles).
        const float inner_w = ImGui::GetContentRegionAvail().x;
        float label_w = 0.0f;
        for (const RoleRow& r : rows) {
            std::string l = RoleRowLabel(r.name);
            if (r.kind == RoleRowKind::Foreign) l += kNotInRoles;
            label_w = std::max(label_w, ImGui::CalcTextSize(l.c_str()).x);
        }
        label_w = std::min(label_w + 12.0f, std::max(80.0f, inner_w * 0.45f));
        const float src_w = ImGui::CalcTextSize("auto").x + 10.0f;
        const float x_w = fh;  // the ✕ column (empty on built-in and foreign rows)
        for (const RoleRow& row : rows) {
            ImGui::PushID(row.key.c_str());
            Role      role;
            const bool builtin = RoleFromKey(row.key, &role);
            int        bone = -1;
            if (builtin) {
                const size_t i = static_cast<size_t>(role);
                bone = i < m.role_to_bone.size() ? m.role_to_bone[i] : -1;
            } else {
                const auto it = m.custom_role_to_bone.find(row.key);
                bone = it != m.custom_role_to_bone.end() ? it->second : -1;
            }
            const bool has_bone = bone >= 0 && bone < static_cast<int>(m.bone_names.size());
            const auto eit = g_roles_entries.find(row.key);
            const StoredRoleEntry e = eit != g_roles_entries.end() ? eit->second : StoredRoleEntry{};
            // A stored bone this rig lacks is still the user's choice ("you"), shown as not found.
            const char* src = has_bone ? (e.stored ? "you" : "auto") : (e.stored && !e.bone.empty() ? "you" : "none");
            const ImU32 src_col = !has_bone ? kBad : e.stored ? ui::kAccent : ui::kMuted;
            const std::string preview = has_bone ? m.bone_names[static_cast<size_t>(bone)]
                                        : (e.stored && e.bone.empty()) ? std::string("\xE2\x80\x94 none \xE2\x80\x94")
                                                                       : std::string("\xE2\x80\x94 not found \xE2\x80\x94");
            const bool renaming = row.kind == RoleRowKind::Custom && row.key == g_roles_rename_key;
            const float x0 = ImGui::GetCursorPosX();
            if (renaming) {
                // In place: Enter applies, Esc (or a click elsewhere) cancels.
                ImGui::SetNextItemWidth(std::max(60.0f, inner_w - src_w - x_w - 12.0f));
                if (g_roles_rename_focus) {
                    ImGui::SetKeyboardFocusHere();
                    g_roles_rename_focus = false;
                }
                const bool enter = ImGui::InputTextWithHint("##rename", "Role name", g_roles_rename_buf,
                                                            sizeof(g_roles_rename_buf),
                                                            ImGuiInputTextFlags_EnterReturnsTrue |
                                                                ImGuiInputTextFlags_AutoSelectAll);
                if (enter) {
                    RolesRename(row.key, g_roles_rename_buf);
                    g_roles_rename_key.clear();
                } else if (ImGui::IsItemDeactivated()) {
                    g_roles_rename_key.clear();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                    ImGui::SetTooltip("Enter renames (its rules and bones keep working), Esc cancels");
            } else {
                // The label: a custom role's is clickable (F2 / right-click renames it).
                std::string label = RoleRowLabel(row.name);
                const ImVec2 lp = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##label", ImVec2(std::max(1.0f, label_w - 8.0f), fh));
                const bool hovered = ImGui::IsItemHovered();
                const ImU32 label_col = row.kind == RoleRowKind::Builtin ? ui::kMuted : ui::kText;
                const float ty = lp.y + (fh - ImGui::GetTextLineHeight()) * 0.5f;
                dl->PushClipRect(lp, ImVec2(lp.x + label_w - 8.0f, lp.y + fh), true);
                dl->AddText(ImVec2(lp.x, ty), label_col, label.c_str());
                if (row.kind == RoleRowKind::Foreign)
                    dl->AddText(ImVec2(lp.x + ImGui::CalcTextSize(label.c_str()).x, ty), ui::kFaint, kNotInRoles);
                dl->PopClipRect();
                if (row.kind == RoleRowKind::Custom) {
                    if (hovered) g_roles_hovered_key = row.key;
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) g_roles_last_custom = row.key;
                    if (ImGui::BeginPopupContextItem("##rowctx")) {
                        g_roles_last_custom = row.key;
                        if (ImGui::MenuItem("Rename", "F2", false, writable)) RolesStartRename(row.key, row.name);
                        if (ImGui::MenuItem("Delete\xE2\x80\xA6", nullptr, false, writable)) {
                            g_roles_rename_key.clear();
                            g_roles_confirm_key = row.key;
                        }
                        ImGui::EndPopup();
                    }
                }
                if (hovered && !ImGui::IsPopupOpen("##rowctx")) {
                    if (row.kind == RoleRowKind::Custom)
                        ImGui::SetTooltip("%s (role:%s)\nYour role, for every project. Right-click or F2 renames it.",
                                          row.name.c_str(), row.key.c_str());
                    else if (row.kind == RoleRowKind::Foreign)
                        ImGui::SetTooltip("%s (role:%s)\nRead by these rules but not in your roles (a colleague's "
                                          "preset?). Map it here, or create it with + Role to name it.",
                                          row.name.c_str(), row.key.c_str());
                }
                ImGui::SameLine(x0 + label_w);
                const float combo_w = std::max(60.0f, ImGui::GetContentRegionAvail().x - src_w - x_w - 12.0f);
                const ImVec2 c0 = ImGui::GetCursorScreenPos();
                ImGui::SetNextItemWidth(combo_w);
                if (!has_bone) ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(kBadText));
                const bool open = ImGui::BeginCombo("##bone", preview.c_str(), ImGuiComboFlags_HeightLargest);
                if (!has_bone) ImGui::PopStyleColor();
                if (open) {
                    RolesMenu(row.key, e);
                    ImGui::EndCombo();
                }
                if (!has_bone)
                    dl->AddRect(c0, ImVec2(c0.x + combo_w, c0.y + fh), WithAlpha(kBad, 0x99),
                                ImGui::GetStyle().FrameRounding);
            }
            ImGui::SameLine(0.0f, 6.0f);
            const ImVec2 sp = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(src_w, fh));
            const float sw = ImGui::CalcTextSize(src).x;
            dl->AddText(ImVec2(sp.x + src_w - sw, sp.y + (fh - ImGui::GetTextLineHeight()) * 0.5f), src_col, src);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                std::string tip;
                if (!has_bone && e.stored && !e.bone.empty())
                    tip = "Your choice, " + e.bone + ", is not a bone of this skeleton.";
                else if (e.stored && has_bone)
                    tip = "Your choice, remembered for every item with this skeleton.";
                else if (has_bone)
                    tip = builtin ? "RAV's guess from the bone names."
                                  : "RAV's guess: named like the bone you picked for this role on another skeleton.";
                else
                    tip = "No bone plays this role.";
                ImGui::SetTooltip("%s", tip.c_str());
            }
            ImGui::SameLine(0.0f, 4.0f);
            if (row.kind == RoleRowKind::Custom) {
                if (IconButton("##rdel", fh, [](ImDrawList* d, ImVec2 c, ImU32 col) { IconCross(d, c, 3.5f, col); },
                               writable)) {
                    g_roles_rename_key.clear();
                    g_roles_confirm_key = row.key;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Delete this role (its bones stay remembered)");
            } else {
                ImGui::Dummy(ImVec2(x_w, fh));
            }
            if (row.kind == RoleRowKind::Custom && row.key == g_roles_confirm_key) {
                bool used = false;
                for (const std::string& k : m.rule_role_keys) used = used || k == row.key;
                RolesDeleteConfirm(row, used, inner_w);
            }
            ImGui::PopID();
        }

        // + Role: a new role, for every project.
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        {
            if (!writable) ImGui::BeginDisabled();
            const float btn_w = ImGui::CalcTextSize("+ Role").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            // 10-3f: Import... / Export... (small buttons) share the row.
            const float io_w = ImGui::CalcTextSize("Import...").x + ImGui::CalcTextSize("Export...").x +
                               ImGui::GetStyle().FramePadding.x * 4.0f + 10.0f;
            ImGui::SetNextItemWidth(std::max(60.0f, inner_w - btn_w - io_w - 6.0f));
            const bool enter = ImGui::InputTextWithHint("##newrole", "New role name, e.g. sword tip", g_roles_new,
                                                        sizeof(g_roles_new), ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("A role of your own (a weapon tip, a hand, a tail...): rules and presets can then "
                                  "name it, and it is mapped per skeleton like the others");
            ImGui::SameLine(0.0f, 6.0f);
            const bool click = ui::SolidButton("+ Role");
            if ((enter || click) && writable) RolesCreate(g_roles_new);  // refused (red line) when empty or taken
            // 10-3f: Import (a write: refused like the others when roles.txt cannot be read).
            ImGui::SameLine(0.0f, 6.0f);
            if (ImGui::SmallButton("Import...##rimport") && writable && !TaggingHasPendingDialog())
                RolesRequestDialog(PendingDialog::Kind::RolesImport, m);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("Reads a role .csv file into your roles and this skeleton's bones (Ctrl+Z undoes it)");
            if (!writable) ImGui::EndDisabled();
            // Export works from what is shown, even when roles.txt cannot be read.
            ImGui::SameLine(0.0f, 4.0f);
            if (ImGui::SmallButton("Export...##rexport") && !TaggingHasPendingDialog())
                RolesRequestDialog(PendingDialog::Kind::RolesExport, m);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("Writes your roles and this skeleton's bones to a .csv file you can send");
        }

        // roles.txt that RAV cannot read, a refused write: in red, never silent.
        ImGui::PushTextWrapPos(0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(kBadText));
        if (m.roles_status == RoleMapStatus::Unreadable)
            ImGui::TextUnformatted("roles.txt could not be read: choices are not saved.");
        else if (m.roles_status == RoleMapStatus::NotRolesFile)
            ImGui::TextUnformatted("roles.txt is not a roles file: your next choice moves it aside and starts a new one.");
        if (!g_roles_err.empty()) ImGui::TextUnformatted(g_roles_err.c_str());
        ImGui::PopStyleColor();
        if (!g_roles_status.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::kMuted));
            ImGui::TextUnformatted(g_roles_status.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();


        if (close) {
            ImGui::CloseCurrentPopup();
            RolesForget();
        }
        ImGui::EndPopup();
    } else {
        RolesForget();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

void ItemOptionsPopup(const ItemRules& rules)
{
    if (!ImGui::BeginPopup("##tagoptions")) return;
    ui::Caption("Item options");
    ImGui::SameLine();
    ImGui::TextDisabled("(all its rules)");
    ImGui::TextUnformatted("Sensitivity");
    ImGui::SameLine(110.0f);
    NumberField("##sens", rules.options.sensitivity * 100.0, 0.5, 0, "%", 70.0f, "RAV: Set sensitivity",
                [](ItemRules& r, double v) {
                    r.options.sensitivity = std::min(1.0, std::max(0.0, v / 100.0));
                    return true;
                });
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Drops events weaker than this share of the clip's strongest (0 = off)");
    ImGui::TextUnformatted("Edge margin");
    ImGui::SameLine(110.0f);
    NumberField("##edge", rules.options.edge_margin_ms, 1.0, 0, "ms", 70.0f, "RAV: Set edge margin",
                [](ItemRules& r, double v) {
                    r.options.edge_margin_ms = std::max(0.0, v);
                    return true;
                });
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Ignores events this close to the clip's start and end (T-pose frames)");
    ImGui::EndPopup();
}

void DrawHeader(const TaggingModel& m, const ItemRules& rules)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fh = ImGui::GetFrameHeight();
    // Item name, Roles, options.
    {
        // 10-3 fb-1: Roles and options show on an item without a record too (a gesture creates it).
        const float right_w = fh + 4.0f + 8.0f + ImGui::CalcTextSize("Roles " RAV_DOT " 00 missing").x + 16.0f;
        const float name_w = std::max(40.0f, ImGui::GetContentRegionAvail().x - right_w);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(name_w, fh));
        dl->PushClipRect(p, ImVec2(p.x + name_w, p.y + fh), true);
        dl->AddText(ImVec2(p.x, p.y + (fh - ImGui::GetTextLineHeight()) * 0.5f), ui::kText, m.item_name.c_str());
        dl->PopClipRect();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s\nThe item under the playhead", m.path.c_str());
        {
            ImGui::SameLine();
            char label[64];
            if (m.missing_count > 0)
                std::snprintf(label, sizeof(label), "Roles " RAV_DOT " %d missing", m.missing_count);
            else
                std::snprintf(label, sizeof(label), "Roles");
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            const float rw = ImGui::CalcTextSize(label).x + (m.missing_count > 0 ? 16.0f : 30.0f);
            // Story 10-3d: it opens the Skeleton & roles window (disabled without a skeleton).
            const bool no_skeleton = !m.file_loaded || m.bone_names.empty();
            if (no_skeleton) ImGui::BeginDisabled();
            if (ImGui::InvisibleButton("##roles", ImVec2(rw, fh))) g_roles_request = true;
            if (no_skeleton) ImGui::EndDisabled();
            if (ImGui::IsItemHovered() && !no_skeleton)
                dl->AddRectFilled(rp, ImVec2(rp.x + rw, rp.y + fh), ui::kHover, ui::kRadiusSm);
            const ImU32 edge = m.missing_count > 0 ? WithAlpha(kBad, 0x99) : ui::kStroke;
            dl->AddRect(rp, ImVec2(rp.x + rw, rp.y + fh), edge, ui::kRadiusSm);
            dl->AddText(ImVec2(rp.x + 8.0f, rp.y + (fh - ImGui::GetTextLineHeight()) * 0.5f),
                        m.missing_count > 0 ? kBadText : ui::kMuted, label);
            const bool no_rule = rules.blocks.empty();  // 10-3 fb-1: nothing to check yet
            if (m.missing_count == 0 && !no_rule)
                IconCheck(dl, ImVec2(rp.x + rw - 12.0f, rp.y + fh * 0.5f), 4.0f, ui::kOk);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (no_skeleton)
                    ImGui::SetTooltip("%s", m.file_loaded ? "This file has no skeleton: no bone to map roles on."
                                                          : "The animation file is not loaded: no skeleton to map roles on.");
                else if (m.missing_count > 0)
                    ImGui::SetTooltip("No bone plays: %s.\nDetection and Commit skip this item until they are mapped."
                                      "\nClick to pick their bones.",
                                      m.missing.c_str());
                else if (no_rule)
                    ImGui::SetTooltip("No rule reads a role yet.\nClick to see which bone plays each role.");
                else
                    ImGui::SetTooltip("Every role these rules read has a bone on this skeleton.\nClick to see or "
                                      "change which bone plays each role.");
            }
            ImGui::SameLine();
            if (IconButton("##opts", fh, [](ImDrawList* d, ImVec2 c, ImU32 col) { IconGear(d, c, 6.0f, col); }))
                ImGui::OpenPopup("##tagoptions");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Item options");
            ItemOptionsPopup(rules);
        }
    }

    // The preset field and its menu, the Legacy band (story 10-3b).
    DrawPresetField(m);

    // The rules.
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    ui::Caption("Rules");
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("+ Rule").x - 16.0f);
    if (ImGui::SmallButton("+ Rule##addrule")) {
        const ImU32 col = NextRuleColor(rules.blocks);
        Edit("RAV: Add rule", [col](ItemRules& r) {
            char name[32];
            std::snprintf(name, sizeof(name), "Rule %d", static_cast<int>(r.blocks.size()) + 1);
            r.blocks.push_back(DefaultBlock(name, ToRecordColor(col)));
            return true;
        });
        g_sel = static_cast<int>(rules.blocks.size());  // the new rule (written at the end of the frame)
    }
    const int nb = static_cast<int>(rules.blocks.size());
    int do_toggle = -1, do_dup = -1, do_del = -1;
    for (int b = 0; b < nb; ++b) {
        const Block& blk = rules.blocks[static_cast<size_t>(b)];
        ImGui::PushID(b);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float rh = fh + 4.0f;
        const bool sel = b == g_sel;
        // The card: the whole row selects; the switch and the buttons sit on it.
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##card", ImVec2(w, rh))) {
            g_sel = b;
            ClearEventSelection();  // the inspector shows the rule
        }
        const bool hov = ImGui::IsItemHovered();
        // 10-3 fb-2: every rule is its own card (filled, edged), a stripe in its colour on its left.
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rh), hov && !sel ? ui::kHover : ui::kRaised, ui::kRadiusMd);
        if (sel) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rh), ui::kAccentSoft, ui::kRadiusMd);
        RuleStripe(dl, p, ImVec2(p.x + w, p.y + rh), blk.enabled ? RuleColor(blk) : WithAlpha(RuleColor(blk), 0x66),
                   ui::kRadiusMd);
        dl->AddRect(p, ImVec2(p.x + w, p.y + rh), sel ? ui::kAccentLine : ui::kStroke, ui::kRadiusMd);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 9.0f, p.y + 2.0f));
        if (Switch("##on", blk.enabled)) do_toggle = b;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip(blk.enabled ? "Switch this rule off" : "Switch this rule on");
        const float ty = p.y + (rh - ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddCircleFilled(ImVec2(p.x + 46.0f, p.y + rh * 0.5f), 4.0f,
                            blk.enabled ? RuleColor(blk) : WithAlpha(RuleColor(blk), 0x66), 12);
        // Count (an off rule: 0, faint), duplicate, delete on the right.
        char count[16];
        if (!blk.enabled)
            std::snprintf(count, sizeof(count), "0");
        else if (m.detected) {
            // Story 10-4: the events Apply writes for this rule (detections kept + the user's own).
            int k = 0;
            for (const PlannedMarker& pm : m.planned)
                if (pm.block == b && pm.in_item) ++k;
            std::snprintf(count, sizeof(count), "%d", k);
        }
        else
            std::snprintf(count, sizeof(count), "-");
        const float bx = p.x + w - 2.0f * (fh + 2.0f) - 4.0f;
        const float cw = ImGui::CalcTextSize(count).x;
        EllipsisText(dl, ImVec2(p.x + 55.0f, ty), bx - cw - 10.0f, blk.enabled ? ui::kText : ui::kFaint, RuleTitle(blk));
        dl->AddText(ImVec2(bx - cw - 6.0f, ty), blk.enabled ? ui::kMuted : ui::kFaint, count);
        ImGui::SetCursorScreenPos(ImVec2(bx, p.y + 2.0f));
        if (IconButton("##dup", fh, [](ImDrawList* d, ImVec2 c, ImU32 col) { IconDuplicate(d, c, 4.5f, col); })) do_dup = b;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Duplicate");
        ImGui::SameLine(0.0f, 2.0f);
        if (IconButton("##del", fh, [](ImDrawList* d, ImVec2 c, ImU32 col) { IconCross(d, c, 4.0f, col); })) do_del = b;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Delete");
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rh + 4.0f));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::PopID();
    }
    if (do_toggle >= 0) {
        const Block& blk = rules.blocks[static_cast<size_t>(do_toggle)];
        const std::string desc = std::string(blk.enabled ? "RAV: Switch rule off (" : "RAV: Switch rule on (") +
                                 RuleTitle(blk) + ")";
        const int b = do_toggle;
        Edit(desc, [b](ItemRules& r) {
            Block* x = BlockAt(r, b);
            if (!x) return false;
            x->enabled = !x->enabled;
            return true;
        });
    } else if (do_dup >= 0) {
        const int b = do_dup;
        const ImU32 col = NextRuleColor(rules.blocks);
        Edit("RAV: Duplicate rule (" + RuleTitle(rules.blocks[static_cast<size_t>(b)]) + ")", [b, col](ItemRules& r) {
            Block* x = BlockAt(r, b);
            if (!x) return false;
            Block copy = *x;
            copy.color = ToRecordColor(col);
            // Story 10-4: the rules after it move down one; their events follow.
            RemapEventBlocks(r.events, BlockMapForInsert(r.blocks.size(), b + 1));
            r.blocks.insert(r.blocks.begin() + b + 1, copy);
            return true;
        });
        ClearEventSelection();
        g_sel = b + 1;
    } else if (do_del >= 0) {
        const int b = do_del;
        Edit("RAV: Delete rule (" + RuleTitle(rules.blocks[static_cast<size_t>(b)]) + ")", [b](ItemRules& r) {
            if (!BlockAt(r, b)) return false;
            // Story 10-4: its user events and suppressions go with it; the next rules' move up.
            RemapEventBlocks(r.events, BlockMapForDelete(r.blocks.size(), b));
            r.blocks.erase(r.blocks.begin() + b);
            return true;
        });
        ClearEventSelection();
        if (g_sel >= b && g_sel > 0) --g_sel;
    }
}

// ---- Panel: inspector ----------------------------------------------------------------------------

void ConditionEditor(const Block& blk, int b, int c)
{
    const Condition& cd = blk.conditions[static_cast<size_t>(c)];
    const std::string rule = RuleTitle(blk);
    // 10-4 follow-up: the kind (Bone / Joint angle / Rotation). Bend / Turn are older
    // flexion / yaw conditions, shown as they are (their bones are not picked here).
    const ConditionKind kind = ConditionKindOf(cd.signal);
    const bool legacy = kind == ConditionKind::Bend || kind == ConditionKind::Turn;
    const float fh = ImGui::GetFrameHeight();
    ImGui::PushID(c);
    // 10-3 fb-2: the condition is a card read top-down as a sentence (decision A):
    //    IF [kind] [bone] +      lock x  /  reads [measure] [axis]  /  from [reference]
    //    goes [direction] [threshold]  /  margin [margin]  -- rows a kind lacks are omitted.
    Card card;
    BeginCard(card);
    Sentence s = MakeSentence(card.p0.x + kCardPad, card.p0.x + card.w - kCardPad);
    // IF / AND, kind, Bone, lock, delete.
    {
        SentenceRow(s, c ? "AND" : "IF", true);
        SentenceChip(s, ChipWidth(ConditionKindLabel(kind), 50.0f));
        if (BeginChip("##kind", ConditionKindLabel(kind), 50.0f)) {
            for (ConditionKind k : kConditionKindChoices)
                if (ImGui::Selectable(ConditionKindLabel(k), k == kind) && k != kind)
                    Edit("RAV: Condition kind (" + rule + ")", [b, c, k](ItemRules& r) {
                        Condition* x = CondAt(r, b, c);
                        if (!x) return false;
                        SetConditionKind(*x, k);
                        // The kept bone applies only when it is a joint here: else the left knee.
                        if (k == ConditionKind::JointAngle && x->signal.bones.size() == 1) {
                            const TaggingModel& tm = GetTaggingModel();
                            const int bone = ResolveOnSkeleton(tm, x->signal.bones[0]);
                            if (bone >= 0 && !IsJointBone(tm, bone))
                                x->signal.bones = {static_cast<int>(Role::LeftKnee)};
                        }
                        return true;
                    });
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip(legacy ? "An older condition: changing its kind starts it over"
                                     : "What this condition reads: a bone's position, the angle at a joint, or a "
                                       "bone's rotation (changing it starts the condition over)");
        const std::string bone = SignalBoneLabel(cd.signal);
        SentenceChip(s, ChipWidth(bone.empty() ? "?" : bone.c_str(), 60.0f));
        if (legacy) ImGui::BeginDisabled();
        const int single = cd.signal.bones.size() == 1 ? cd.signal.bones[0] : -1;
        if (BeginChip("##bone", bone.empty() ? "?" : bone.c_str(), 60.0f, legacy)) {
            const int id = BoneMenuItems(single, kind == ConditionKind::JointAngle);
            ImGui::EndCombo();
            if (id >= 0) {
                Edit("RAV: Condition bone (" + rule + ")", [b, c, id](ItemRules& r) {
                    Condition* x = CondAt(r, b, c);
                    if (!x) return false;
                    const ConditionKind k = ConditionKindOf(x->signal);
                    if (k == ConditionKind::Bend || k == ConditionKind::Turn) return false;
                    x->signal.bones = {id};
                    x->signal.combine = Combine::Single;
                    x->signal.bone_floors.clear();
                    return true;
                });
            }
        }
        if (legacy) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(legacy ? "An older angle reads several bones: they are not picked here (another kind starts over)"
                              : kind == ConditionKind::JointAngle
                                  ? "The joint: RAV reads the angle between its parent and its child (180 = straight)"
                              : kind == ConditionKind::Rotation ? "The bone whose rotation this condition reads"
                                                                : "The bone this condition reads: a role, or a bone of this skeleton");
        // Spec 10-3c -- "+" puts the bone selected in the 3D view (Skeleton mode): its role when
        // it plays exactly one on this skeleton (rules stay portable), else the raw bone. With
        // none selected, it switches the view to Skeleton (nothing written).
        {
            const TaggingModel& tm = GetTaggingModel();
            const std::string&  sel_bone = SkeletonSelectedBone();
            int         put = -1;
            std::string what;
            std::string why;  // non-empty: refused
            if (!sel_bone.empty()) {
                int sel_idx = -1;
                for (size_t i = 0; i < tm.bone_names.size() && sel_idx < 0; ++i)
                    if (tm.bone_names[i] == sel_bone) sel_idx = static_cast<int>(i);
                put = BoneRefForPickedBone(sel_idx, sel_bone, tm.role_to_bone, tm.custom_role_to_bone);
                what = put == BoneRefForBone(sel_bone) ? sel_bone : BoneRefLabel(put) + " (" + sel_bone + ")";
                if (sel_idx < 0)
                    why = sel_bone + " is not on this item's skeleton";
                else if (kind == ConditionKind::JointAngle && !IsJointBone(tm, sel_idx))
                    why = sel_bone + " has no parent or no child: no joint angle there";
            }
            const bool enabled = !legacy && why.empty();
            SentenceChip(s, fh, 2.0f);
            if (IconButton("##usebone", fh, [](ImDrawList* d, ImVec2 cc, ImU32 col) { IconPlus(d, cc, 4.0f, col); },
                           enabled)) {
                if (sel_bone.empty()) {
                    SetSkeletonModeOn(true);
                } else {
                    Edit("RAV: Condition bone (" + rule + ")", [b, c, put](ItemRules& r) {
                        Condition* x = CondAt(r, b, c);
                        if (!x) return false;
                        const ConditionKind k = ConditionKindOf(x->signal);
                        if (k == ConditionKind::Bend || k == ConditionKind::Turn) return false;
                        if (x->signal.bones.size() == 1 && x->signal.bones[0] == put &&
                            x->signal.combine == Combine::Single && x->signal.bone_floors.empty())
                            return false;  // already this bone: nothing to write
                        x->signal.bones = {put};
                        x->signal.combine = Combine::Single;
                        x->signal.bone_floors.clear();
                        return true;
                    });
                }
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (legacy)
                    ImGui::SetTooltip("An older angle: its bones are not picked here");
                else if (sel_bone.empty())
                    ImGui::SetTooltip("Select a bone in the 3D view (this switches it to Skeleton), then + uses it");
                else if (!why.empty())
                    ImGui::SetTooltip("%s", why.c_str());
                else
                    ImGui::SetTooltip("Use %s", what.c_str());
            }
        }
        // Lock and delete, right-aligned in the card (under the chips when they do not fit).
        const int nconds = static_cast<int>(blk.conditions.size());
        const float right = (nconds > 1 ? 2.0f : 1.0f) * (fh + 2.0f) - 2.0f;
        {
            const float rx = std::max(s.chips_x, s.right - right);
            if (ImGui::GetItemRectMax().x + SentenceGap() <= rx) ImGui::SameLine();
            ImGui::SetCursorScreenPos(ImVec2(rx, ImGui::GetCursorScreenPos().y));
        }
        const bool fixed = !cd.auto_threshold;
        if (IconButton("##lock", fh, [fixed](ImDrawList* d, ImVec2 cc, ImU32 col) {
                IconLock(d, cc, 4.5f, fixed ? ui::kAccent : col, fixed);
            })) {
            Edit(std::string(fixed ? "RAV: Unlock threshold (" : "RAV: Lock threshold (") + rule + ")", [b, c](ItemRules& r) {
                Condition* x = CondAt(r, b, c);
                if (!x) return false;
                x->auto_threshold = !x->auto_threshold;
                return true;
            });
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip(fixed ? "Fixed: Analyse keeps this threshold and margin" : "Analyse may change this threshold");
        if (nconds > 1) {
            ImGui::SameLine(0.0f, 2.0f);
            if (IconButton("##rmc", fh, [](ImDrawList* d, ImVec2 cc, ImU32 col) { IconCross(d, cc, 4.0f, col); })) {
                Edit("RAV: Delete condition (" + rule + ")", [b, c](ItemRules& r) {
                    Block* x = BlockAt(r, b);
                    if (!x || x->conditions.size() < 2 || c >= static_cast<int>(x->conditions.size())) return false;
                    RemoveCondition(*x, static_cast<size_t>(c));
                    return true;
                });
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Remove this condition");
        }
    }
    // reads [Measure] [axis]  /  from [reference]   --   reads [Measure] (angle: no from)
    {
        SentenceRow(s, "reads");
        SentenceChip(s, ChipWidth(MeasureLabelFor(cd.signal, cd.signal.measure), 70.0f));
        if (BeginChip("##measure", MeasureLabelFor(cd.signal, cd.signal.measure), 70.0f)) {
            for (Measure ms : kMeasureChoices)
                if (ImGui::Selectable(MeasureLabelFor(cd.signal, ms), ms == cd.signal.measure) && ms != cd.signal.measure)
                    Edit("RAV: Condition measure (" + rule + ")", [b, c, ms](ItemRules& r) {
                        Condition* x = CondAt(r, b, c);
                        if (!x) return false;
                        // A rotation's angle has no total: it reads as X (the axis chip shows it).
                        if (x->signal.quantity == Quantity::Rotation)
                            x->signal.axis = RotationAxisOf(x->signal);
                        x->signal.measure = ms;
                        if (x->signal.quantity == Quantity::Rotation && !RotationAxisOffered(ms, x->signal.axis))
                            x->signal.axis = Axis::X;
                        return true;
                    });
            ImGui::EndCombo();
        }
        if (legacy) {
            const char* what = kind == ConditionKind::Bend ? "(bend)" : "(turn)";
            SentenceChip(s, ImGui::CalcTextSize(what).x);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", what);
        } else if (kind == ConditionKind::JointAngle) {
            // The angle at the joint: no axis, no reference.
        } else if (kind == ConditionKind::Rotation) {
            const Axis cur = RotationAxisOf(cd.signal);
            SentenceChip(s, ChipWidth(AxisLabel(cur), 50.0f));
            if (BeginChip("##raxis", AxisLabel(cur), 50.0f)) {
                for (Axis a : kRotationAxisChoices) {
                    if (!RotationAxisOffered(cd.signal.measure, a)) continue;
                    if (ImGui::Selectable(AxisLabel(a), a == cur) && a != cur)
                        Edit("RAV: Condition axis (" + rule + ")", [b, c, a](ItemRules& r) {
                            Condition* x = CondAt(r, b, c);
                            if (!x) return false;
                            x->signal.axis = a;
                            return true;
                        });
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("The rotation axis (rotate order XYZ, like a 3D tool's rotate channels); total = "
                                  "the overall turning rate");
            const char* ref = RotationReferenceLabel(cd.signal.reference);
            SentenceRow(s, "from");
            SentenceChip(s, ChipWidth(ref, 70.0f));
            if (BeginChip("##rref", ref, 70.0f)) {
                for (Reference rf : {Reference::Parent, Reference::Floor})
                    if (ImGui::Selectable(RotationReferenceLabel(rf), rf == cd.signal.reference) &&
                        rf != cd.signal.reference)
                        Edit("RAV: Condition reference (" + rule + ")", [b, c, rf](ItemRules& r) {
                            Condition* x = CondAt(r, b, c);
                            if (!x) return false;
                            x->signal.reference = rf;
                            x->signal.ref_bones.clear();
                            return true;
                        });
                ImGui::EndCombo();
            }
        } else {
            SentenceChip(s, ChipWidth(AxisLabel(cd.signal.axis), 60.0f));
            if (BeginChip("##axis", AxisLabel(cd.signal.axis), 60.0f)) {
                for (Axis a : kAxisChoices)
                    if (ImGui::Selectable(AxisLabel(a), a == cd.signal.axis) && a != cd.signal.axis)
                        Edit("RAV: Condition axis (" + rule + ")", [b, c, a](ItemRules& r) {
                            Condition* x = CondAt(r, b, c);
                            if (!x) return false;
                            x->signal.axis = a;
                            return true;
                        });
                ImGui::EndCombo();
            }
            const std::string ref = cd.signal.reference == Reference::Floor ? std::string("the floor")
                                                                            : RefLabel(cd.signal.ref_bones);
            SentenceRow(s, "from");
            SentenceChip(s, ChipWidth(ref.c_str(), 70.0f));
            if (BeginChip("##ref", ref.c_str(), 70.0f)) {
                int picked = -2;
                if (ImGui::Selectable("the floor", cd.signal.reference == Reference::Floor)) picked = -1;
                ImGui::Separator();
                const int single = (cd.signal.reference == Reference::Bones && cd.signal.ref_bones.size() == 1)
                                       ? cd.signal.ref_bones[0]
                                       : -3;
                const int id = BoneMenuItems(single);
                if (id >= 0) picked = id;
                ImGui::EndCombo();
                if (picked != -2)
                    Edit("RAV: Condition reference (" + rule + ")", [b, c, picked](ItemRules& r) {
                        Condition* x = CondAt(r, b, c);
                        if (!x) return false;
                        if (picked == -1) {
                            x->signal.reference = Reference::Floor;
                            x->signal.ref_bones.clear();
                        } else {
                            x->signal.reference = Reference::Bones;
                            x->signal.ref_bones = {picked};
                            x->signal.ref_combine = Combine::Average;
                            x->signal.bone_floors.clear();
                        }
                        return true;
                    });
            }
        }
        // goes [below|above] N unit  /  margin N unit
        SentenceRow(s, "goes");
        SentenceChip(s, ChipWidth(DirectionLabel(cd.dir), 56.0f));
        if (BeginChip("##dir", DirectionLabel(cd.dir), 56.0f)) {
            for (Direction d : kDirectionChoices)
                if (ImGui::Selectable(DirectionLabel(d), d == cd.dir) && d != cd.dir)
                    Edit("RAV: Condition direction (" + rule + ")", [b, c, d](ItemRules& r) {
                        Condition* x = CondAt(r, b, c);
                        if (!x) return false;
                        x->dir = d;
                        return true;
                    });
            ImGui::EndCombo();
        }
        const SignalUnit u = UnitOf(cd.signal);
        SentenceChip(s, NumberFieldWidth(64.0f, UnitLabel(u)));
        NumberField("##thr", ToDisplay(cd.signal, cd.threshold), UnitDragStep(u), UnitDecimals(u), UnitLabel(u), 64.0f,
                    "RAV: Set threshold (" + rule + ")", [b, c](ItemRules& r, double v) {
                        Condition* x = CondAt(r, b, c);
                        if (!x) return false;
                        x->threshold = FromDisplay(x->signal, v);
                        return true;
                    });
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Threshold: drag sideways (Shift: fine), click to type");
        SentenceRow(s, "margin");
        SentenceChip(s, NumberFieldWidth(64.0f, UnitLabel(u)));
        NumberField("##margin", ToDisplay(cd.signal, std::max(0.0, cd.margin)), UnitDragStep(u), UnitDecimals(u),
                    UnitLabel(u), 64.0f, "RAV: Set margin (" + rule + ")", [b, c](ItemRules& r, double v) {
                        Condition* x = CondAt(r, b, c);
                        if (!x) return false;
                        x->margin = std::max(0.0, FromDisplay(x->signal, v));
                        return true;
                    });
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Hysteresis: how far back past the threshold before it can fire again");
    }
    EndCard(card, ui::kSurface, ui::kStroke);
    ImGui::PopID();
}

// Story 10-4 -- the selected event: its state, time, values and rule; Suppress / Restore /
// Delete; Go to event. A user event's time is typed here (ms in the clip): it moves there.
void DrawEventInspector(const TaggingModel& m, const ItemRules& rules, const ShownEvent& e)
{
    const Block& blk = rules.blocks[static_cast<size_t>(e.block)];
    const std::string rule = RuleTitle(blk);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Crumb: < rule  >  (dot) Event rule
    {
        const std::string back = "< " + rule + "##evback";
        if (ImGui::SmallButton(back.c_str())) ClearEventSelection();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Back to the rule");
        ImGui::SameLine();
        ImGui::TextDisabled(">");
        ImGui::SameLine();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  lh = ImGui::GetTextLineHeight();
        dl->AddCircleFilled(ImVec2(p.x + 4.0f, p.y + lh * 0.5f), 4.0f, RuleColor(blk), 12);
        ImGui::Dummy(ImVec2(10.0f, lh));
        ImGui::SameLine();
        ImGui::TextUnformatted(("Event " + rule).c_str());
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    const char* state = e.kind == ShownKind::Detected     ? "Detected"
                        : e.kind == ShownKind::User       ? "Yours (fixed time)"
                        : e.kind == ShownKind::Suppressed ? "Suppressed"
                                                          : "Suppression (nothing detected here now)";
    // Values: a detection's own; a user event's measured at its time, as detection measures them.
    double strength = e.strength, speed = e.speed;
    if (e.kind == ShownKind::User) TaggingMeasureEvent(rules, e.block, e.t, &strength, &speed);
    SignalSpec speed_spec = blk.conditions.empty() ? SignalSpec{} : blk.conditions[0].signal;
    speed_spec.measure = Measure::Speed;
    SignalSpec strength_spec = blk.strength_signal.bones.empty() ? speed_spec : blk.strength_signal;

    constexpr float kKeyW = 78.0f;
    auto key = [&](const char* k) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", k);
        ImGui::SameLine(kKeyW);
    };
    key("State");
    ImGui::TextUnformatted(state);

    key("Time");
    double project_t = 0.0;
    const bool on_timeline = FirstPassProjectTime(m.map, e.t, &project_t);
    if (e.kind == ShownKind::User && e.entry >= 0) {
        // Typed (or dragged sideways): previews live, Enter / release writes one undo point.
        const int    entry = e.entry, block = e.block;
        const double clip_len = m.map.clip_len > 0.0 ? m.map.clip_len : 1e9;
        double ms = e.t * 1000.0;
        const double saved_t = (entry < static_cast<int>(m.rules.events.size()) &&
                                m.rules.events[static_cast<size_t>(entry)].kind == EventKind::User)
                                   ? m.rules.events[static_cast<size_t>(entry)].t
                                   : e.t;
        switch (ui::DragNumber("##evtime", &ms, 1.0, 1, "ms", 90.0f)) {
        case ui::DragNumberEvent::Live: {
            const double t = std::min(std::max(0.0, ms / 1000.0), clip_len);
            Later([entry, block, t, item = m.item]() {
                if (GetTaggingModel().item != item) return;
                ItemRules p = TaggingShownRules();
                if (entry >= static_cast<int>(p.events.size())) return;
                EventEntry& x = p.events[static_cast<size_t>(entry)];
                if (x.kind != EventKind::User || x.block != block) return;
                x.t = t;
                TaggingPreview(p, true);  // an event's time: its commit auto-applies
            });
            g_sel_ev.t = t;
            break;
        }
        case ui::DragNumberEvent::Commit: {
            const double t = std::min(std::max(0.0, ms / 1000.0), clip_len);
            if (t != saved_t) {
                EventEdit("RAV: Set event time (" + rule + ")", [block, saved_t, t](ItemRules& r) {
                    const int i = FindEntry(r, EventKind::User, block, saved_t);
                    if (i < 0) return false;
                    EventEntry& x = r.events[static_cast<size_t>(i)];
                    x.t = t;
                    TaggingMeasureEvent(r, block, t, &x.strength, &x.speed);
                    x.has_strength = x.has_speed = true;
                    return true;
                });
            } else {
                Later([]() { TaggingCancelPreview(); });
            }
            SelectEventAt(block, t, EvGroup::User);
            break;
        }
        case ui::DragNumberEvent::Cancel:
            Later([]() { TaggingCancelPreview(); });
            g_sel_ev.t = saved_t;
            break;
        default:
            break;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Its time in the clip: drag sideways, or click to type (Enter)");
    } else {
        ImGui::Text("%.1f ms", e.t * 1000.0);
    }
    {
        char tl[48];
        if (on_timeline) FormatTime(project_t, tl, sizeof(tl));
        ImGui::SetCursorPosX(kKeyW);
        if (on_timeline) ImGui::TextDisabled("at %s on the timeline", tl);
        else ImGui::TextDisabled("outside the item: not written");
    }

    if (e.kind != ShownKind::Orphan) {
        key("Strength");
        ImGui::TextUnformatted(FormatDisplay(strength_spec, strength).c_str());
        key("Speed");
        ImGui::TextUnformatted(FormatDisplay(speed_spec, speed).c_str());
    }
    key("Rule");
    ImGui::TextUnformatted(rule.c_str());

    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    ui::SubText(e.kind == ShownKind::User       ? "Detection never moves your events. Their values are measured at their time."
                : e.kind == ShownKind::Detected ? "Drag it in the strip to make it yours, or suppress it."
                                                : "Masks any detection of this rule within 30 ms of this time.");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    const char* dk = ShortcutKeyLabel(kShortcutTagDelEvent);
    const std::string act = std::string(e.kind == ShownKind::Detected ? "Suppress" : e.kind == ShownKind::User ? "Delete" : "Restore") +
                            "  (" + dk + ")##evact";
    if (ui::SolidButton(act.c_str())) {
        const ShownEvent copy = e;
        ToggleEvent(copy);
    }
    ImGui::SameLine();
    if (!on_timeline) ImGui::BeginDisabled();
    if (ImGui::Button("Go to event##evgo")) {
        const double t = project_t;
        QueueVideoSeek(t);
    }
    if (!on_timeline) ImGui::EndDisabled();
}

// One button per preset, each loading it on the current item (a shortcut: rules can also be
// built by hand with + Rule).
void DrawPresetButtons()
{
    const std::vector<PresetInfo>& presets = CachedPresets();
    if (presets.empty()) ui::SubText("No preset found.");
    for (const PresetInfo& p : presets) {
        ImGui::PushID(p.id.c_str());
        const std::string label = p.name + (p.factory ? "  (factory)" : "");
        if (ui::SolidButton(label.c_str(), ImVec2(-1.0f, 0.0f))) {
            const std::string id = p.id;
            Later([id]() { TaggingLoadPreset(id); });
        }
        ImGui::PopID();
    }
}

void DrawInspector(const ItemRules& rules)
{
    const int nb = static_cast<int>(rules.blocks.size());
    if (g_sel < 0 || g_sel >= nb) {
        ui::SubText("No rule. Add one (+ Rule) and pick its bone and signal, or start from a preset:");
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        DrawPresetButtons();
        return;
    }
    // Story 10-4: the inspector shows the selected event, when there is one.
    {
        const TaggingModel& m = GetTaggingModel();
        const ShownEvent* ev = SelectedEvent(m);
        if (ev && ev->block >= 0 && ev->block < nb) {
            DrawEventInspector(m, rules, *ev);
            return;
        }
    }
    const int b = g_sel;
    const Block& blk = rules.blocks[static_cast<size_t>(b)];
    const std::string rule = RuleTitle(blk);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fh = ImGui::GetFrameHeight();

    // Colour dot + marker name.
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##swatch", ImVec2(fh, fh))) ImGui::OpenPopup("##palette");
        dl->AddCircleFilled(ImVec2(p.x + fh * 0.5f, p.y + fh * 0.5f), fh * 0.32f, FromRecordColor(blk.color), 16);
        if (ImGui::IsItemHovered()) dl->AddCircle(ImVec2(p.x + fh * 0.5f, p.y + fh * 0.5f), fh * 0.40f, ui::kStrokeStrong, 16, 1.5f);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Marker colour");
        if (ImGui::BeginPopup("##palette")) {
            for (int i = 0; i < 8; ++i) {
                ImGui::PushID(i);
                const ImVec2 q = ImGui::GetCursorScreenPos();
                const uint32_t rc = ToRecordColor(ui::kCurvePalette[i]);
                if (ImGui::InvisibleButton("##col", ImVec2(20.0f, 20.0f)) && rc != blk.color)
                    Edit("RAV: Rule colour (" + rule + ")", [b, rc](ItemRules& r) {
                        Block* x = BlockAt(r, b);
                        if (!x) return false;
                        x->color = rc;
                        return true;
                    });
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(q.x + 10.0f, q.y + 10.0f), 7.0f, ui::kCurvePalette[i], 16);
                if (rc == blk.color)
                    ImGui::GetWindowDrawList()->AddCircle(ImVec2(q.x + 10.0f, q.y + 10.0f), 9.0f, ui::kText, 16, 1.5f);
                if (i < 7) ImGui::SameLine(0.0f, 4.0f);
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        // The field shows the saved name whenever it is not being typed into. While it is,
        // the buffer stays the typed text of the rule it was opened on (g_name_block), even if
        // another rule gets selected meanwhile: the name is written to that rule.
        if (!g_name_active) {
            std::snprintf(g_name_buf, sizeof(g_name_buf), "%s", blk.marker.c_str());
            g_name_block = b;
            g_name_item = GetTaggingModel().item;
        }
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##markername", g_name_buf, sizeof(g_name_buf));
        g_name_active = ImGui::IsItemActive();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The marker's name on the timeline");
        if (ImGui::IsItemDeactivatedAfterEdit() && !ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            const std::string name = g_name_buf;
            const int nb = g_name_block;
            const Block* target = (nb >= 0 && nb < static_cast<int>(rules.blocks.size()) && g_name_item == GetTaggingModel().item)
                                      ? &rules.blocks[static_cast<size_t>(nb)]
                                      : nullptr;
            if (target && name != target->marker)
                Edit("RAV: Rename rule (" + RuleTitle(*target) + ")",
                     [nb, name](ItemRules& r) {
                         Block* x = BlockAt(r, nb);
                         if (!x) return false;
                         x->marker = name;
                         return true;
                     },
                     g_name_item);
        }
    }

    // When all of these hold.
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ui::Caption("When all of these hold");
    for (int c = 0; c < static_cast<int>(blk.conditions.size()); ++c) ConditionEditor(blk, b, c);
    if (ImGui::SmallButton("+ AND condition")) {
        Edit("RAV: Add condition (" + rule + ")", [b](ItemRules& r) {
            Block* x = BlockAt(r, b);
            if (!x) return false;
            x->conditions.push_back(DefaultCondition());
            return true;
        });
    }

    // Place the marker at [the start | the highest point | the lowest point] of [signal].
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ui::Caption("Place the marker");
    // 10-3 fb-2: the same label column and chip edge as the condition cards (decision A).
    const float sx0 = ImGui::GetCursorScreenPos().x + kCardPad;
    const float sx1 = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - kCardPad;
    {
        const Placement pl = PlacementOf(blk);
        Sentence sen = MakeSentence(sx0, sx1);
        SentenceRow(sen, "at");
        SentenceChip(sen, ChipWidth(PlacementLabel(pl), 80.0f));
        if (BeginChip("##place", PlacementLabel(pl), 80.0f)) {
            for (Placement q : kPlacementChoices)
                if (ImGui::Selectable(PlacementLabel(q), q == pl) && q != pl) {
                    const int of = blk.peak_condition;
                    Edit("RAV: Marker placement (" + rule + ")", [b, q, of](ItemRules& r) {
                        Block* x = BlockAt(r, b);
                        if (!x) return false;
                        SetPlacement(*x, q, of);
                        return true;
                    });
                }
            ImGui::EndCombo();
        }
        SentenceRow(sen, "of");
        if (pl == Placement::Start) {
            SentenceChip(sen, ImGui::CalcTextSize("the match").x);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("the match");
        } else {
            const int of = std::min(std::max(0, blk.peak_condition), static_cast<int>(blk.conditions.size()) - 1);
            const std::string cur =
                of >= 0 ? SignalName(blk.conditions[static_cast<size_t>(of)].signal,
                                     SignalBoneLabel(blk.conditions[static_cast<size_t>(of)].signal))
                        : std::string("?");
            SentenceChip(sen, ChipWidth(cur.c_str(), 80.0f));
            if (BeginChip("##placeof", cur.c_str(), 80.0f)) {
                for (int c = 0; c < static_cast<int>(blk.conditions.size()); ++c) {
                    const SignalSpec& s = blk.conditions[static_cast<size_t>(c)].signal;
                    ImGui::PushID(c);
                    if (ImGui::Selectable(SignalName(s, SignalBoneLabel(s)).c_str(), c == of) && c != of)
                        Edit("RAV: Marker placement (" + rule + ")", [b, c, pl](ItemRules& r) {
                            Block* x = BlockAt(r, b);
                            if (!x) return false;
                            SetPlacement(*x, pl, c);
                            return true;
                        });
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
        }
        ui::SubText("The match is the stretch where all the conditions hold. A footstep: the highest point of the "
                    "knee bend speed.");
    }
    // Offset, Min length, Cooldown (ms).
    {
        struct Timing {
            const char* label;
            const char* id;
            const char* tip;
            double value;
            int which;
        };
        const Timing rows[3] = {
            {"Offset", "##offs", "Moves every marker of this rule, e.g. +30 ms if the sound lands after the contact",
             blk.offset_ms, 0},
            {"Min length", "##hold", "The match must last at least this long to count", blk.min_hold_ms, 1},
            {"Cooldown", "##cool", "No new marker from this rule until this time has passed", blk.cooldown_ms, 2},
        };
        Sentence sen = MakeSentence(sx0, sx1);
        for (const Timing& t : rows) {
            SentenceRow(sen, t.label);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", t.tip);
            SentenceChip(sen, NumberFieldWidth(70.0f, "ms"));
            const int which = t.which;
            NumberField(t.id, t.value, 1.0, 0, "ms", 70.0f, std::string("RAV: Set ") + (which == 0 ? "offset" : which == 1 ? "min length" : "cooldown") + " (" + rule + ")",
                        [b, which](ItemRules& r, double v) {
                            Block* x = BlockAt(r, b);
                            if (!x) return false;
                            if (which == 0) x->offset_ms = v;
                            else if (which == 1) x->min_hold_ms = std::max(0.0, v);
                            else x->cooldown_ms = std::max(0.0, v);
                            return true;
                        });
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", t.tip);
        }
    }
}

// ---- Panel: footer ---------------------------------------------------------------------------------

void DrawFooter(const TaggingModel& m)
{
    ImGui::Separator();
    // Which markers Apply writes (the global option) and its "Change" (opens the menu on it).
    {
        const MarkerMode mode = GetTaggingMarkerMode();
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::kMuted));
        ImGui::TextUnformatted(MarkerModeLine(mode));
        ImGui::PopStyleColor();
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 6.0f,
                                 ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("Change").x - 12.0f));
        if (ImGui::SmallButton("Change##tagmarkmode")) g_menu_request = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Take, project or both: a global option in the menu (Auto-Tagging options).\n"
                              "Project markers take the rule's colour.");
    }
    // Commit (Apply renamed, 10-4 fb-4): every selected item that has rules, one undo point.
    // Cancel beside it: the same items' previews deleted, their rules back to the last Commit.
    const int run = std::max(0, m.sel_count - m.sel_without_rules - m.sel_roles_skipped);
    const int with_rules = m.sel_cancellable;
    {
        const float cancel_w = ImGui::CalcTextSize("Cancel").x + ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f;
        const float commit_w =
            std::max(60.0f, ImGui::GetContentRegionAvail().x - cancel_w - ImGui::GetStyle().ItemSpacing.x);
        char label[64];
        const bool nothing = run > 0 && m.sel_to_commit == 0;  // every item's markers are up to date
        if (nothing) std::snprintf(label, sizeof(label), "Nothing to commit##tagapply");
        else std::snprintf(label, sizeof(label), "Commit to %d item%s##tagapply", run, run == 1 ? "" : "s");
        if (run == 0 || nothing) ImGui::BeginDisabled();
        if (ui::PrimaryButton(label, ImVec2(commit_w, 0.0f))) Later([]() { CommitTaggingMarkers(); });
        if (run == 0 || nothing) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(run == 0  ? "Select the items to tag (items with rules) in REAPER."
                              : nothing ? "The selected items' markers are up to date: no preview to commit."
                                        : "Writes the markers of every selected item that has rules, each with its own "
                                         "rules,\nand removes their previews. Replaces only RAV's markers. One undo point.");
        ImGui::SameLine();
        if (with_rules == 0) ImGui::BeginDisabled();
        if (ui::SolidButton("Cancel##tagcancel", ImVec2(-1.0f, 0.0f))) Later([]() { CancelTaggingChanges(); });
        if (with_rules == 0) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(with_rules == 0 ? "Select the items to tag (items with rules) in REAPER."
                                              : "Deletes the preview markers of every selected item that has rules and "
                                                "puts\ntheir rules, thresholds and events back as they were at the last "
                                                "Commit. One undo point.");
    }
    // The selection, and whether this item's markers are written.
    {
        std::string line = std::to_string(m.sel_count) + " selected";
        if (m.sel_without_rules > 0) line += " " RAV_DOT " " + std::to_string(m.sel_without_rules) + " without rules skipped";
        ImGui::PushTextWrapPos(0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::kMuted));
        ImGui::TextUnformatted(line.c_str());
        ImGui::PopStyleColor();
        if (m.sel_roles_skipped > 0) {
            ImGui::SameLine(0.0f, 4.0f);
            char roles[64];
            std::snprintf(roles, sizeof(roles), RAV_DOT " %d skipped: roles", m.sel_roles_skipped);
            ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(kBadText));
            ImGui::TextUnformatted(roles);
            ImGui::PopStyleColor();
        }
        if (m.detected && !m.rules.blocks.empty()) {  // 10-3 fb-1: no rule = nothing to write, no warning
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(m.markers_up_to_date ? ui::kMuted : ui::kWarn));
            ImGui::TextUnformatted(m.markers_up_to_date ? RAV_DOT " markers up to date"
                                   : m.has_previews     ? RAV_DOT " preview not committed"
                                                        : RAV_DOT " markers not written yet");
            ImGui::PopStyleColor();
        }
        // The last Commit or Cancel outcome.
        const ApplyResult& ar = LastApplyResult();
        if (!ar.error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(kBadText));
            ImGui::TextUnformatted(ar.error.c_str());
            ImGui::PopStyleColor();
        } else if (ar.ran) {
            std::string out = std::string(ar.cancel ? "Changes cancelled on " : "Markers committed on ") +
                              std::to_string(ar.items) + " item" + (ar.items == 1 ? "" : "s");
            if (ar.already_present > 0)
                out += " " RAV_DOT " " + std::to_string(ar.already_present) + " already present, left as is";
            if (ar.failed > 0) out += " " RAV_DOT " " + std::to_string(ar.failed) + " could not be written";
            ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::kMuted));
            ImGui::TextUnformatted(out.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();
    }
}

}  // namespace

bool TaggingRolesWindowOpen()
{
    // Only while it is drawn (another view or the viewer closing drops it).
    if (!g_roles_open || !ImGui::GetCurrentContext()) return false;
    const int age = ImGui::GetFrameCount() - g_roles_drawn_frame;
    return age >= 0 && age <= 1;
}

void DrawTaggingPanel(float x, float y, float w, float h)
{
    const TaggingModel& m = GetTaggingModel();
    if (m.item != g_sel_item) OnItemChanged(m.item);
    const ItemRules& rules = TaggingShownRules();
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::Col(ui::kSurface));
    ImGui::PushStyleColor(ImGuiCol_Border, ui::Col(ui::kStroke));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##tagpanel", nullptr, kFlags)) {
        // The left edge (6 px) drags the panel's width (280-600 px), kept on release.
        {
            const ImVec2 content = ImGui::GetCursorScreenPos();
            const ImVec2 wp = ImGui::GetWindowPos();
            ImGui::SetCursorScreenPos(ImVec2(wp.x - 3.0f, wp.y + 8.0f));
            ImGui::InvisibleButton("##panelgrip", ImVec2(6.0f, std::max(1.0f, h - 16.0f)));
            const float mx = ImGui::GetIO().MousePos.x;
            if (ImGui::IsItemActivated()) {
                g_panel_drag = true;
                g_panel_press_x = mx;
                g_panel_start_w = PanelWidth();
            }
            if (ImGui::IsItemActive() && g_panel_drag)
                g_panel_w = std::floor(std::min(std::max(g_panel_start_w - (mx - g_panel_press_x), kPanelMinW), kPanelMaxW));
            if (ImGui::IsItemDeactivated() && g_panel_drag) {
                g_panel_drag = false;
                SavePrefFloat(kPrefPanelWidth, PanelWidth());
            }
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x + 0.5f, wp.y + 8.0f), ImVec2(wp.x + 0.5f, wp.y + h - 8.0f),
                                                    ui::kAccentLine, 2.0f);
            }
            ImGui::SetCursorScreenPos(content);
        }

        if (!m.item) {
            ui::SubText("No animation item under the playhead");
        } else if (m.unreadable) {
            // 10-3 fb-1: only a record RAV cannot read stays behind a preset load (a hand edit
            // would write over data RAV does not understand). No record = the editor, empty.
            ImGui::TextUnformatted(m.item_name.c_str());
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            DrawPresetField(m);  // story 10-3b: the menu loads, imports, exports (Save needs rules)
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            ImGui::TextUnformatted("No rules");
            ui::SubText("This item's rules could not be read. They are kept as they are until you load a preset.");
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            DrawPresetButtons();
            if (!TaggingLastError().empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(kBadText));
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(TaggingLastError().c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
        } else {
            if (g_sel >= static_cast<int>(rules.blocks.size())) g_sel = static_cast<int>(rules.blocks.size()) - 1;
            if (g_sel < 0) g_sel = 0;
            DrawHeader(m, rules);
            if (!TaggingLastError().empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(kBadText));
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(TaggingLastError().c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::SmallButton("OK##tagerr")) TaggingClearError();
            }
            ImGui::Separator();
            // The inspector scrolls; the footer stays.
            // Story 10-4: the markers line, Commit / Cancel, the selection and the last outcome.
            const float footer_h = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetTextLineHeightWithSpacing() * 3.0f + 10.0f;
            const float body_h = std::max(40.0f, ImGui::GetContentRegionAvail().y - footer_h);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::Col(ui::kSurface));
            if (ImGui::BeginChild("##taginspector", ImVec2(0.0f, body_h), ImGuiChildFlags_None)) {
                if (m.missing_count > 0) {
                    char line[200];
                    std::snprintf(line, sizeof(line),
                                  "This skeleton is missing %d role%s (%s). The rules come back as soon as they are mapped.",
                                  m.missing_count, m.missing_count == 1 ? "" : "s", m.missing.c_str());
                    ui::SubText(line);
                    ImGui::Dummy(ImVec2(0.0f, 4.0f));
                }
                DrawInspector(rules);
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
            DrawFooter(m);
        }
        RolesWindow(m);  // story 10-3d (closes itself when the item changes)
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    RunLater();
}

}  // namespace rav

#endif  // _WIN32
