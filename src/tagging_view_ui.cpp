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
#include "item_rules.h"
#include "preset_store.h"
#include "reaper_api.h"
#include "rule_record.h"
#include "shortcuts.h"  // LoadPrefFloat / SavePrefFloat
#include "strip_view.h"
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

// A drag in the strip's lanes: a threshold or re-arm line, or a scrub.
enum class StripDrag { None, Scrub, Threshold, Margin };
StripDrag g_drag = StripDrag::None;
int       g_drag_block = -1;
int       g_drag_cond = -1;
bool      g_drag_moved = false;

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
    Later([desc, fn, item]() { TaggingEdit(desc.c_str(), fn, item); });
}

std::string RuleTitle(const Block& b)
{
    return b.marker.empty() ? std::string("(unnamed rule)") : b.marker;
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
    return g_drag == StripDrag::Threshold || g_drag == StripDrag::Margin || ui::DragNumberActive();
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
            const std::vector<ClipPass> passes = ClipPasses(m.map);
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
                    if (blk.enabled && b < static_cast<int>(m.trace.blocks.size())) {
                        const ImU32 col = RuleColor(blk);
                        for (const Event& e : m.trace.blocks[static_cast<size_t>(b)].events) {
                            for (double pt : ClipToProjectTimes(m.map, e.time_s)) {
                                if (pt < win.v0 || pt > win.v1) continue;
                                const float ex = std::floor(x_of(pt)) + 0.5f;
                                dl->AddLine(ImVec2(ex, ry + 2.0f), ImVec2(ex, ry + row_h - 2.0f), col, 1.5f);
                                dl->AddTriangleFilled(ImVec2(ex - 4.5f, ry + 2.0f), ImVec2(ex + 4.5f, ry + 2.0f),
                                                      ImVec2(ex, ry + 8.0f), col);
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
                    const bool now_in = ProjectToClipTime(m.map, playhead, &clip_now);
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
                if (area_pressed) {
                    g_drag = StripDrag::Scrub;
                    g_drag_moved = false;
                    int cond = -1;
                    bool margin = false;
                    if (line_at(mouse.x, mouse.y, &cond, &margin)) {
                        g_drag = margin ? StripDrag::Margin : StripDrag::Threshold;
                        g_drag_block = g_sel;
                        g_drag_cond = cond;
                    } else {
                        for (int b = 0; b < nb; ++b) {
                            const float ry = row_y[static_cast<size_t>(b)];
                            if (mouse.y >= ry && mouse.y <= ry + row_h) {
                                g_sel = b;
                                if (mouse.x < lane_x) g_drag = StripDrag::None;  // the row's label: select only
                                break;
                            }
                        }
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
                    if (line_at(mouse.x, mouse.y, &cond, &margin)) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                    } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && mouse.x >= lane_x) {
                        ImGui::SetTooltip("Click or drag to move the playhead\nDrag a threshold line (dashed: the re-arm "
                                          "level) to tune it\nAlt+wheel: zoom  " RAV_DOT "  Shift+wheel: scroll");
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
        if (ImGui::InvisibleButton("##card", ImVec2(w, rh))) g_sel = b;
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
        else if (b < static_cast<int>(m.trace.blocks.size()) && m.detected)
            std::snprintf(count, sizeof(count), "%d", static_cast<int>(m.trace.blocks[static_cast<size_t>(b)].events.size()));
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
            r.blocks.insert(r.blocks.begin() + b + 1, copy);
            return true;
        });
        g_sel = b + 1;
    } else if (do_del >= 0) {
        const int b = do_del;
        Edit("RAV: Delete rule (" + RuleTitle(rules.blocks[static_cast<size_t>(b)]) + ")", [b](ItemRules& r) {
            if (!BlockAt(r, b)) return false;
            r.blocks.erase(r.blocks.begin() + b);
            return true;
        });
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

void DrawInspector(const ItemRules& rules)
{
    const int nb = static_cast<int>(rules.blocks.size());
    if (g_sel < 0 || g_sel >= nb) {
        ui::SubText("No rule. Add one (+ Rule), or pick a preset.");
        return;
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
    // Apply is shown, disabled until story 10-4 (the mock-up's disabled primary: raised, faint).
    ImGui::PushStyleColor(ImGuiCol_Button, ui::Col(ui::kRaised));
    ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::kFaint));
    ImGui::BeginDisabled();
    ImGui::Button("Apply##tagapply", ImVec2(-1.0f, 0.0f));
    ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Writing the markers comes with story 10-4");
    ui::Caption("Markers: story 10-4");
    char sel[160];
    std::snprintf(sel, sizeof(sel), "%d selected " RAV_DOT " %d without rules skipped " RAV_DOT " %d skipped: roles",
                  m.sel_count, m.sel_without_rules, m.sel_roles_skipped);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::kMuted));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(sel);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
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
            const float footer_h = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() * 3.0f + 8.0f;
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
