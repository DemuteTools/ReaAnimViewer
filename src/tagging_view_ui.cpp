// SPDX-License-Identifier: MIT
//
// See tagging_view_ui.h.

#include "tagging_view_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

#include "bone_roles.h"
#include "event_list.h"
#include "item_rules.h"
#include "preset_store.h"
#include "reaper_api.h"
#include "rule_record.h"
#include "shortcuts.h"  // LoadPrefFloat / SavePrefFloat
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
    Later([desc, fn, item]() {
        if (TaggingEdit(desc.c_str(), fn, item)) ClearLastApplyResult();  // the footer's Apply line is stale now
    });
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
    Edit("RAV: Add event (" + RuleNameAt(shown, block) + ")", [block, t](ItemRules& r) {
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
        Edit("RAV: Suppress event (" + rule + ")", [block, t](ItemRules& r) {
            if (!BlockAt(r, block)) return false;
            r.events.push_back(MakeSuppression(block, t));
            return true;
        });
    } else if (e.kind == ShownKind::Suppressed || e.kind == ShownKind::Orphan) {
        const double t0 = EntryTime(e);
        if (!std::isfinite(t0)) return;
        Edit("RAV: Restore event (" + rule + ")", [block, t0](ItemRules& r) {
            const int i = FindEntry(r, EventKind::Suppress, block, t0);
            if (i < 0) return false;
            r.events.erase(r.events.begin() + i);
            return true;
        });
    } else if (e.kind == ShownKind::User) {
        const double t0 = EntryTime(e);
        if (!std::isfinite(t0)) return;
        Edit("RAV: Delete event (" + rule + ")", [block, t0](ItemRules& r) {
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
        Edit("RAV: Move event (" + rule + ")", [block, det_t, to](ItemRules& r) {
            if (!BlockAt(r, block)) return false;
            double s = 0.0, v = 0.0;
            TaggingMeasureEvent(r, block, to, &s, &v);
            r.events.push_back(MakeSuppression(block, det_t));
            r.events.push_back(MakeUserEvent(block, to, s, v));
            return true;
        });
    } else if (e.kind == ShownKind::User) {
        if (!std::isfinite(entry_t)) return;
        Edit("RAV: Move event (" + rule + ")", [block, entry_t, to](ItemRules& r) {
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
    return m.item && m.has_rules && m.detected && m.missing_count == 0;
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
        dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), c, selected ? 2.5f : 1.5f);
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

// The Bone menu's list: the roles, then the raw bones. Returns the picked id, or -1.
int BoneMenuItems(int current_single)
{
    int picked = -1;
    ImGui::TextDisabled("Roles");
    for (int r = 0; r < static_cast<int>(Role::Count); ++r) {
        const bool sel = current_single == r;
        if (ImGui::Selectable(RoleShortLabel(static_cast<Role>(r)).c_str(), sel)) picked = r;
    }
    const TaggingModel& m = GetTaggingModel();
    if (!m.bone_names.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Bones");
        for (size_t i = 0; i < m.bone_names.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const int id = BoneRefForBone(m.bone_names[i]);
            if (ImGui::Selectable(m.bone_names[i].c_str(), current_single == id)) picked = id;
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

// A small combo ("chip", the mock-up's select.chip) sized to its preview text, a small
// chevron on its right.
bool BeginChip(const char* id, const char* preview, float min_w = 40.0f, bool disabled = false)
{
    const float w = std::min(ChipWidth(preview, min_w), std::max(min_w, ImGui::GetContentRegionAvail().x));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(w);
    const bool open = ImGui::BeginCombo(id, preview, ImGuiComboFlags_HeightLarge | ImGuiComboFlags_NoArrowButton);
    const ImVec2 c(p0.x + w - 10.0f, p0.y + h * 0.5f);
    dl->AddTriangleFilled(ImVec2(c.x - 3.5f, c.y - 1.5f), ImVec2(c.x + 3.5f, c.y - 1.5f), ImVec2(c.x, c.y + 2.5f),
                          disabled ? ui::kFaint : ui::kMuted);
    return open;
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
        float hx = c0.x + std::max(64.0f, ImGui::CalcTextSize(tc).x) + 12.0f;
        const bool has_sel = m.item && m.has_rules && g_sel < static_cast<int>(rules.blocks.size());
        if (has_sel) {
            const Block& b = rules.blocks[static_cast<size_t>(g_sel)];
            dl->AddCircleFilled(ImVec2(hx + 4.0f, c0.y + head_h * 0.5f), 4.0f, RuleColor(b), 12);
            const std::string title = RuleTitle(b);
            dl->AddText(ImVec2(hx + 14.0f, c0.y + (head_h - th) * 0.5f), ui::kText, title.c_str());
            char sub[64];
            std::snprintf(sub, sizeof(sub), " " RAV_DOT " %d signal%s", static_cast<int>(b.conditions.size()),
                          b.conditions.size() == 1 ? "" : "s");
            dl->AddText(ImVec2(hx + 14.0f + ImGui::CalcTextSize(title.c_str()).x, c0.y + (head_h - th) * 0.5f),
                        ui::kMuted, sub);
        }
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
            const bool can = m.item && m.has_rules && m.missing_count == 0 && m.file_loaded && !rules.blocks.empty();
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
        } else if (!m.has_rules) {
            msg1 = m.unreadable ? "This item's rules could not be read." : "No rules on this item.";
            msg2 = "Pick a preset in the panel.";
        } else if (m.missing_count > 0) {
            std::snprintf(miss_line, sizeof(miss_line), "%d role%s no bone on this skeleton: no signal to draw.",
                          m.missing_count, m.missing_count == 1 ? " has" : "s have");
            msg1 = miss_line;
            msg_col = kBadText;
            miss_names = "Missing: " + m.missing + ". Apply skips this item until they have a bone.";
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
                std::vector<float> row_y(static_cast<size_t>(nb));
                for (int b = 0; b < nb; ++b) {
                    const Block& blk = rules.blocks[static_cast<size_t>(b)];
                    const float ry = rows_top + 2.0f + static_cast<float>(b) * (row_h + 2.0f);
                    row_y[static_cast<size_t>(b)] = ry;
                    const bool sel = b == g_sel;
                    dl->AddRectFilled(ImVec2(c0.x, ry), ImVec2(lane_x + lane_w, ry + row_h),
                                      sel ? IM_COL32(0x5B, 0x8C, 0xFF, 0x1A) : ui::kBg, ui::kRadiusSm);
                    dl->AddRect(ImVec2(c0.x, ry), ImVec2(lane_x + lane_w, ry + row_h), sel ? ui::kAccentLine : ui::kStroke,
                                ui::kRadiusSm);
                    dl->AddCircleFilled(ImVec2(c0.x + 9.0f, ry + row_h * 0.5f), 3.5f, RuleColor(blk), 12);
                    const std::string title = RuleTitle(blk);
                    dl->PushClipRect(ImVec2(c0.x, ry), ImVec2(lane_x - 4.0f, ry + row_h), true);
                    dl->AddText(ImVec2(c0.x + 18.0f, ry + (row_h - th) * 0.5f),
                                blk.enabled ? (sel ? ui::kText : ui::kMuted) : ui::kFaint, title.c_str());
                    dl->PopClipRect();
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
                }
                const float rows_bottom = rows_top + 2.0f + static_cast<float>(nb) * (row_h + 2.0f);
                dim_outside(rows_top, rows_bottom);

                // ---- The selected rule's lanes ----
                std::vector<LaneGeom> lanes;
                const Block* sel_blk = (g_sel < nb) ? &rules.blocks[static_cast<size_t>(g_sel)] : nullptr;
                const BlockTrace* bt = (sel_blk && g_sel < static_cast<int>(m.trace.blocks.size()))
                                           ? &m.trace.blocks[static_cast<size_t>(g_sel)]
                                           : nullptr;
                const float lanes_top = rows_bottom + 4.0f;
                if (sel_blk && bt && bt->ran && lanes_top < area1.y - 20.0f) {
                    const int nc = static_cast<int>(sel_blk->conditions.size());
                    const float gap = 4.0f;
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

                        dl->AddRectFilled(ImVec2(lane_x, L.y0), ImVec2(lane_x + lane_w, L.y1), ui::kBg, ui::kRadiusSm);
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
                        dl->AddRect(ImVec2(lane_x, L.y0), ImVec2(lane_x + lane_w, L.y1), ui::kStroke, ui::kRadiusSm);
                        dl->AddRectFilled(ImVec2(lane_x - 2.0f, yt - 4.0f), ImVec2(lane_x + 6.0f, yt + 4.0f), rc, 2.0f);

                        // The gutter: the signal's name, its condition, its value now.
                        dl->PushClipRect(ImVec2(c0.x, L.y0), ImVec2(lane_x - 4.0f, L.y1), true);
                        const std::string name = std::string(c ? "AND " : "IF ") +
                                                 SignalName(cd.signal, SignalBoneLabel(cd.signal)) +
                                                 ((sel_blk->landing == Landing::PeakOf && sel_blk->peak_condition == c)
                                                      ? (sel_blk->peak_max ? "  (peak)" : "  (low)")
                                                      : "");
                        dl->AddText(ImVec2(c0.x + 4.0f, L.y0 + 2.0f), ui::kText, name.c_str());
                        std::string cond_text = std::string(cd.dir == Direction::Below ? "< " : "> ") +
                                                FormatDisplay(cd.signal, cd.threshold);
                        if (m_ > 0.0) cond_text += "  margin " + FormatDisplay(cd.signal, m_);
                        if (L.y1 - L.y0 > th * 2.0f + 4.0f) dl->AddText(ImVec2(c0.x + 4.0f, L.y0 + 3.0f + th), rc, cond_text.c_str());
                        if (now_in && L.y1 - L.y0 > th * 3.0f + 6.0f && !curve.empty()) {
                            const double pos = clip_now * rate_hz;
                            const double v_now = InterpSample(curve, pos);
                            const size_t hi_ = static_cast<size_t>(std::min<double>(static_cast<double>(curve.size() - 1),
                                                                                    std::max(0.0, std::round(pos))));
                            const bool in_now = hi_ < hold.size() && hold[hi_];
                            const std::string now_text = "now " + FormatDisplay(cd.signal, v_now);
                            dl->AddText(ImVec2(c0.x + 4.0f, L.y0 + 4.0f + 2.0f * th), in_now ? ui::kText : ui::kFaint,
                                        now_text.c_str());
                        }
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
                        if (ClipTimeForDrop(m.map, t_mouse, &to)) g_drag_ev_to = to;
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
                    } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && mouse.x >= lane_x) {
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
            StripScrub(g_view, area_active && g_drag == StripDrag::Scrub && mouse.x >= lane_x, area_pressed, mouse.x,
                       t_mouse, &QueueVideoSeek);
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

void PresetMenuItems()
{
    const std::vector<PresetInfo>& presets = CachedPresets();
    if (presets.empty()) {
        ImGui::TextDisabled("No preset found");
        return;
    }
    bool factory_caption = false, user_caption = false;
    for (const PresetInfo& p : presets) {
        if (p.factory && !factory_caption) {
            ImGui::TextDisabled("Factory");
            factory_caption = true;
        }
        if (!p.factory && !user_caption) {
            if (factory_caption) ImGui::Separator();
            ImGui::TextDisabled("User");
            user_caption = true;
        }
        ImGui::PushID(p.id.c_str());
        if (ImGui::Selectable(p.name.c_str())) {
            const std::string id = p.id;
            Later([id]() { TaggingLoadPreset(id); });
        }
        ImGui::PopID();
    }
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
        const float right_w = (m.has_rules ? fh + 4.0f : 0.0f) + 8.0f +
                              (m.has_rules ? ImGui::CalcTextSize("Roles " RAV_DOT " 00 missing").x + 16.0f : 0.0f);
        const float name_w = std::max(40.0f, ImGui::GetContentRegionAvail().x - right_w);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(name_w, fh));
        dl->PushClipRect(p, ImVec2(p.x + name_w, p.y + fh), true);
        dl->AddText(ImVec2(p.x, p.y + (fh - ImGui::GetTextLineHeight()) * 0.5f), ui::kText, m.item_name.c_str());
        dl->PopClipRect();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s\nThe item under the playhead", m.path.c_str());
        if (m.has_rules) {
            ImGui::SameLine();
            char label[64];
            if (m.missing_count > 0)
                std::snprintf(label, sizeof(label), "Roles " RAV_DOT " %d missing", m.missing_count);
            else
                std::snprintf(label, sizeof(label), "Roles");
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            const float rw = ImGui::CalcTextSize(label).x + (m.missing_count > 0 ? 16.0f : 30.0f);
            ImGui::InvisibleButton("##roles", ImVec2(rw, fh));
            const ImU32 edge = m.missing_count > 0 ? WithAlpha(kBad, 0x99) : ui::kStroke;
            dl->AddRect(rp, ImVec2(rp.x + rw, rp.y + fh), edge, ui::kRadiusSm);
            dl->AddText(ImVec2(rp.x + 8.0f, rp.y + (fh - ImGui::GetTextLineHeight()) * 0.5f),
                        m.missing_count > 0 ? kBadText : ui::kMuted, label);
            if (m.missing_count == 0)
                IconCheck(dl, ImVec2(rp.x + rw - 12.0f, rp.y + fh * 0.5f), 4.0f, ui::kOk);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                if (m.missing_count > 0)
                    ImGui::SetTooltip("No bone plays: %s.\nDetection and Apply skip this item until they are mapped.",
                                      m.missing.c_str());
                else
                    ImGui::SetTooltip("Every role these rules read has a bone on this skeleton.");
            }
            ImGui::SameLine();
            if (IconButton("##opts", fh, [](ImDrawList* d, ImVec2 c, ImU32 col) { IconGear(d, c, 6.0f, col); }))
                ImGui::OpenPopup("##tagoptions");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Item options");
            ItemOptionsPopup(rules);
        }
    }

    // The preset field: Preset: [padlock] Footsteps  edited / Legacy. Load only (10-3b: the menu).
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        if (ImGui::InvisibleButton("##presetfield", ImVec2(w, fh))) {
            CachedPresets(/*refresh=*/true);  // the menu opens on the files as they are now
            ImGui::OpenPopup("##tagpresets");
        }
        const bool hov = ImGui::IsItemHovered();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + fh), hov ? ui::kHover : ui::kRaised, ui::kRadiusSm);
        dl->AddRect(p, ImVec2(p.x + w, p.y + fh), ui::kStroke, ui::kRadiusSm);
        const float ty = p.y + (fh - ImGui::GetTextLineHeight()) * 0.5f;
        float x = p.x + 8.0f;
        dl->AddText(ImVec2(x, ty), ui::kMuted, "Preset:");
        x += ImGui::CalcTextSize("Preset:").x + 6.0f;
        if (m.has_preset && m.preset_factory) {
            IconLock(dl, ImVec2(x + 4.0f, p.y + fh * 0.5f + 1.0f), 4.0f, ui::kMuted, true);
            x += 12.0f;
        }
        const char* name = m.has_preset ? m.preset_name.c_str() : "none";
        const char* tag = !m.has_preset                               ? ""
                          : m.preset_state == PresetState::Legacy     ? "Legacy"
                          : m.preset_state == PresetState::Edited     ? "edited"
                                                                      : "";
        const float tag_w = tag[0] ? ImGui::CalcTextSize(tag).x + 12.0f : 0.0f;
        dl->PushClipRect(ImVec2(x, p.y), ImVec2(p.x + w - tag_w - 18.0f, p.y + fh), true);
        dl->AddText(ImVec2(x, ty), ui::kText, name);
        dl->PopClipRect();
        if (tag[0])
            dl->AddText(ImVec2(p.x + w - tag_w - 14.0f, ty), m.preset_state == PresetState::Legacy ? ui::kWarn : ui::kFaint,
                        tag);
        // The chevron.
        const ImVec2 cc(p.x + w - 10.0f, p.y + fh * 0.5f);
        dl->AddTriangleFilled(ImVec2(cc.x - 3.5f, cc.y - 2.0f), ImVec2(cc.x + 3.5f, cc.y - 2.0f), ImVec2(cc.x, cc.y + 2.5f),
                              ui::kMuted);
        if (hov && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip(m.preset_factory ? "A factory preset (read-only). Click to load another."
                                               : "Click to load a preset on this item");
        ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.0f), ImVec2(std::max(w, 200.0f), 360.0f));
        if (ImGui::BeginPopup("##tagpresets")) {
            PresetMenuItems();
            ImGui::EndPopup();
        }
    }

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
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rh), sel ? ui::kAccentSoft : (hov ? ui::kRaised : 0), ui::kRadiusMd);
        if (sel) dl->AddRect(p, ImVec2(p.x + w, p.y + rh), ui::kAccentLine, ui::kRadiusMd);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 6.0f, p.y + 2.0f));
        if (Switch("##on", blk.enabled)) do_toggle = b;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip(blk.enabled ? "Switch this rule off" : "Switch this rule on");
        const float ty = p.y + (rh - ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddCircleFilled(ImVec2(p.x + 44.0f, p.y + rh * 0.5f), 4.0f, RuleColor(blk), 12);
        // Count (an off rule: 0, faint), duplicate, delete on the right.
        char count[16];
        if (!blk.enabled)
            std::snprintf(count, sizeof(count), "0");
        else if (m.detected) {
            // Story 10-4: the events Apply writes for this rule (detections kept + the user's own).
            int k = 0;
            for (const PlannedMarker& pm : m.planned)
                if (pm.block == b) ++k;
            std::snprintf(count, sizeof(count), "%d", k);
        }
        else
            std::snprintf(count, sizeof(count), "-");
        const float bx = p.x + w - 2.0f * (fh + 2.0f) - 4.0f;
        const float cw = ImGui::CalcTextSize(count).x;
        dl->PushClipRect(ImVec2(p.x + 52.0f, p.y), ImVec2(bx - cw - 10.0f, p.y + rh), true);
        dl->AddText(ImVec2(p.x + 52.0f, ty), blk.enabled ? ui::kText : ui::kFaint, RuleTitle(blk).c_str());
        dl->PopClipRect();
        dl->AddText(ImVec2(bx - cw - 6.0f, ty), blk.enabled ? ui::kMuted : ui::kFaint, count);
        ImGui::SetCursorScreenPos(ImVec2(bx, p.y + 2.0f));
        if (IconButton("##dup", fh, [](ImDrawList* d, ImVec2 c, ImU32 col) { IconDuplicate(d, c, 4.5f, col); })) do_dup = b;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Duplicate");
        ImGui::SameLine(0.0f, 2.0f);
        if (IconButton("##del", fh, [](ImDrawList* d, ImVec2 c, ImU32 col) { IconCross(d, c, 4.0f, col); })) do_del = b;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Delete");
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rh + 2.0f));
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
    const bool angle = IsAngleSignal(cd.signal);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fh = ImGui::GetFrameHeight();
    ImGui::PushID(c);
    // IF / AND, Bone, lock, delete.
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const char* tag = c ? "AND" : "IF";
        const ImVec2 ts = ImGui::CalcTextSize(tag);
        const float tw = ts.x + 8.0f;
        ImGui::Dummy(ImVec2(tw, fh));
        dl->AddRectFilled(ImVec2(p.x, p.y + 3.0f), ImVec2(p.x + tw, p.y + fh - 3.0f), ui::kBg, 4.0f);
        dl->AddRect(ImVec2(p.x, p.y + 3.0f), ImVec2(p.x + tw, p.y + fh - 3.0f), ui::kStroke, 4.0f);
        dl->AddText(ImVec2(p.x + 4.0f, p.y + (fh - ts.y) * 0.5f), ui::kMuted, tag);
        ImGui::SameLine();
        const std::string bone = SignalBoneLabel(cd.signal);
        if (angle) ImGui::BeginDisabled();
        const int single = cd.signal.bones.size() == 1 ? cd.signal.bones[0] : -1;
        if (BeginChip("##bone", bone.empty() ? "?" : bone.c_str(), 60.0f, angle)) {
            const int id = BoneMenuItems(single);
            ImGui::EndCombo();
            if (id >= 0) {
                Edit("RAV: Condition bone (" + rule + ")", [b, c, id](ItemRules& r) {
                    Condition* x = CondAt(r, b, c);
                    if (!x || IsAngleSignal(x->signal)) return false;
                    x->signal.bones = {id};
                    x->signal.combine = Combine::Single;
                    x->signal.bone_floors.clear();
                    return true;
                });
            }
        }
        if (angle) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(angle ? "An angle reads several bones (picking them comes with the skeleton view)"
                                    : "The bone this condition reads: a role, or a bone of this skeleton");
        // Lock and delete, right-aligned.
        const int nconds = static_cast<int>(blk.conditions.size());
        const float right = (nconds > 1 ? 2.0f : 1.0f) * (fh + 2.0f);
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - right));
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
    // [Measure] [axis] from [reference]   /   [Measure] (angle)
    {
        ImGui::Indent(8.0f);
        if (BeginChip("##measure", MeasureLabel(cd.signal.measure), 70.0f)) {
            for (Measure ms : kMeasureChoices)
                if (ImGui::Selectable(MeasureLabel(ms), ms == cd.signal.measure) && ms != cd.signal.measure)
                    Edit("RAV: Condition measure (" + rule + ")", [b, c, ms](ItemRules& r) {
                        Condition* x = CondAt(r, b, c);
                        if (!x) return false;
                        x->signal.measure = ms;
                        return true;
                    });
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (angle) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("(angle)");
        } else {
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
            // "from [reference]" stays on the line when it fits, else it wraps.
            const float from_w = ImGui::CalcTextSize("from").x + ImGui::GetStyle().ItemSpacing.x * 2.0f +
                                 ChipWidth(ref.c_str(), 70.0f);
            ImGui::SameLine();
            if (ImGui::GetContentRegionAvail().x < from_w) ImGui::NewLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("from");
            ImGui::SameLine();
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
        // goes [below|above] N unit . margin N unit
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("goes");
        ImGui::SameLine();
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
        ImGui::SameLine();
        NumberField("##thr", ToDisplay(cd.signal, cd.threshold), UnitDragStep(u), UnitDecimals(u), UnitLabel(u), 64.0f,
                    "RAV: Set threshold (" + rule + ")", [b, c](ItemRules& r, double v) {
                        Condition* x = CondAt(r, b, c);
                        if (!x) return false;
                        x->threshold = FromDisplay(x->signal, v);
                        return true;
                    });
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Threshold: drag sideways (Shift: fine), click to type");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(RAV_DOT " margin");
        ImGui::SameLine();
        NumberField("##margin", ToDisplay(cd.signal, std::max(0.0, cd.margin)), UnitDragStep(u), UnitDecimals(u),
                    UnitLabel(u), 64.0f, "RAV: Set margin (" + rule + ")", [b, c](ItemRules& r, double v) {
                        Condition* x = CondAt(r, b, c);
                        if (!x) return false;
                        x->margin = std::max(0.0, FromDisplay(x->signal, v));
                        return true;
                    });
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Hysteresis: how far back past the threshold before it can fire again");
        ImGui::Unindent(8.0f);
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
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
                TaggingPreview(p);
            });
            g_sel_ev.t = t;
            break;
        }
        case ui::DragNumberEvent::Commit: {
            const double t = std::min(std::max(0.0, ms / 1000.0), clip_len);
            if (t != saved_t) {
                Edit("RAV: Set event time (" + rule + ")", [block, saved_t, t](ItemRules& r) {
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

void DrawInspector(const ItemRules& rules)
{
    const int nb = static_cast<int>(rules.blocks.size());
    if (g_sel < 0 || g_sel >= nb) {
        ui::SubText("No rule. Add one (+ Rule), or pick a preset.");
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
    {
        const Placement pl = PlacementOf(blk);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("at");
        ImGui::SameLine();
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
        ImGui::SameLine();
        if (pl == Placement::Start) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("of the match");
        } else {
            const int of = std::min(std::max(0, blk.peak_condition), static_cast<int>(blk.conditions.size()) - 1);
            const std::string cur =
                of >= 0 ? SignalName(blk.conditions[static_cast<size_t>(of)].signal,
                                     SignalBoneLabel(blk.conditions[static_cast<size_t>(of)].signal))
                        : std::string("?");
            // "of [signal]" wraps when it does not fit on the line.
            if (ImGui::GetContentRegionAvail().x <
                ImGui::CalcTextSize("of").x + ImGui::GetStyle().ItemSpacing.x * 2.0f + ChipWidth(cur.c_str(), 80.0f))
                ImGui::NewLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("of");
            ImGui::SameLine();
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
        for (const Timing& t : rows) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(t.label);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", t.tip);
            ImGui::SameLine(90.0f);
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
    // Apply: every selected item that has rules, one undo point.
    const int run = std::max(0, m.sel_count - m.sel_without_rules - m.sel_roles_skipped);
    {
        char label[64];
        std::snprintf(label, sizeof(label), "Apply to %d item%s##tagapply", run, run == 1 ? "" : "s");
        if (run == 0) ImGui::BeginDisabled();
        if (ui::PrimaryButton(label, ImVec2(-1.0f, 0.0f))) Later([]() { ApplyTaggingMarkers(); });
        if (run == 0) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(run == 0 ? "Select the items to tag (items with rules) in REAPER."
                                       : "Writes the markers of every selected item that has rules, each with its own "
                                         "rules.\nReplaces only RAV's markers. One undo point.");
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
        if (m.detected) {
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(m.markers_up_to_date ? ui::kMuted : ui::kWarn));
            ImGui::TextUnformatted(m.markers_up_to_date ? RAV_DOT " markers up to date" : RAV_DOT " markers not written yet");
            ImGui::PopStyleColor();
        }
        // The last Apply's outcome.
        const ApplyResult& ar = LastApplyResult();
        if (!ar.error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(kBadText));
            ImGui::TextUnformatted(ar.error.c_str());
            ImGui::PopStyleColor();
        } else if (ar.ran) {
            std::string out = "Markers written on " + std::to_string(ar.items) + " item" + (ar.items == 1 ? "" : "s");
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
        } else if (!m.has_rules) {
            ImGui::TextUnformatted(m.item_name.c_str());
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            ImGui::TextUnformatted("No rules");
            ui::SubText(m.unreadable ? "This item's rules could not be read. They are kept as they are until you load "
                                       "a preset."
                                     : "Load a preset to tag this item:");
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
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
            // Story 10-4: the markers line, Apply, the selection and the last Apply's outcome.
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
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    RunLater();
}

}  // namespace rav

#endif  // _WIN32
