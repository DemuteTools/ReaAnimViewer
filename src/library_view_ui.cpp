// SPDX-License-Identifier: MIT
//
// See library_view_ui.h.

#include "library_view_ui.h"
#include "watchdog.h"  // freeze audit: breadcrumbs and scopes

#include "library_view_keys.h"

#ifdef _WIN32

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <map>

#include <imgui.h>

#include "library_media_ui.h"
#include "library_node_ui.h"
#include "library_tree_ui.h"
#include "library_user_ui.h"
#include "assign_view_model.h"
#include "reaper_api.h"
#include "shortcuts.h"
#include "sound_assign.h"
#include "sound_populate.h"
#include "sound_project.h"
#include "sound_record.h"
#include "tagging_assign_ui.h"  // 13-43: the Roles card's Assign opens its Overview
#include "ui_flow.h"
#include "ui_theme.h"
#include "undo_guard.h"

namespace rav {
namespace {

// DECISIONS.md section 1: default widths 16 / 22 / 32 / 30 %. Minimums lowered (13-7a bug 1) to
// 150 / 190 / 260 / 240 px: the column contents wrap and ellipsize down to these, so a narrow
// REAPER dock keeps all four columns instead of scrolling them off.
constexpr float kColumnShare[4] = {0.16f, 0.22f, 0.32f, 0.30f};
// 13-14 (N3): the sizes below are mockup px at the screen's DPI (ui::DpPx reads as the scaled float).
constexpr ui::DpPx kColumnMin[4] = {{150.0f}, {190.0f}, {260.0f}, {240.0f}};
constexpr ui::DpPx kColumnGap{8.0f};  // 13-7 (audit A2 / B13): the mockup's 8 px grid gap = the splitter's hit
constexpr ui::DpPx kStripWidth{28.0f};
constexpr double kToastSeconds = 5.0;
constexpr const char* kWidthKeys[4] = {"lib_col_w0", "lib_col_w1", "lib_col_w2", "lib_col_w3"};

struct ViewState {
    bool  loaded = false;
    float share[4] = {kColumnShare[0], kColumnShare[1], kColumnShare[2], kColumnShare[3]};
    bool  col1_collapsed = false;
    bool  media_collapsed = false;
    bool  focused = false;
};
ViewState g_view;

struct Toast {
    std::string text;
    bool        undo = false;
    std::string undo_desc;   // the gesture's undo point description
    int         state = -1;  // the project state right after the gesture (read when the toast is shown)
    double      until = 0.0;
    std::function<void()> local_undo;  // 13-28: an Undo that is not REAPER's (the user's presets)
};
Toast g_toast;

void LoadPrefs()
{
    if (g_view.loaded) return;
    g_view.loaded = true;
    for (int i = 0; i < 4; ++i) {
        const float v = LoadPrefFloat(kWidthKeys[i], kColumnShare[i]);
        g_view.share[i] = (v > 0.01f && v < 0.95f) ? v : kColumnShare[i];
    }
    g_view.col1_collapsed = LoadPrefBool("lib_col1_collapsed", false);
    g_view.media_collapsed = LoadPrefBool("lib_media_collapsed", false);
}

void SaveWidths()
{
    for (int i = 0; i < 4; ++i) SavePrefFloat(kWidthKeys[i], g_view.share[i]);
}

void ResetWidths()
{
    for (int i = 0; i < 4; ++i) g_view.share[i] = kColumnShare[i];
    SaveWidths();
}

bool Collapsed(int i) { return (i == 0 && g_view.col1_collapsed) || (i == 3 && g_view.media_collapsed); }

// 13-43 (mockup maquette-overview-depuis-library.html, A): a library's root shows its Roles: the body parts it
// offers to serve (its kit, a chip per part, toggled as the Kit... menu), where it plays now, and Assign, which
// opens the Overview of Assign sounds on it.
void DrawLibraryRolesSection()
{
    const SoundStore&   store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, SelectedLibraryUid());
    if (!lib || lib->root.empty() || LibraryRowNode() != lib->root) return;
    const std::string uid = lib->uid;
    const char*       kAssign = "Assign \xE2\x86\x97##libroles";
    ui::SectionOpts   opts;
    opts.foldable = false;
    opts.right_w = ui::ButtonWidth(kAssign);
    ui::Section sec;
    const bool  open = ui::BeginSection(sec, "Roles", opts);
    ui::BeginSectionHeaderRight(sec);
    if (ui::SolidButton(kAssign)) OpenAssignOverview(uid);
    ui::ItemTipText("Open Assign sounds on its Overview (Project): the body parts and characters this library plays are lit");
    ui::EndSectionHeaderRight(sec);
    if (open) {
        // Kit: a chip per body part, wrapping with the column.
        ui::LabelRow("Kit", ui::Dp(120.0f));
        const float x0 = ImGui::GetCursorScreenPos().x;
        const float wrap = std::max(ui::Dp(60.0f), ImGui::GetContentRegionAvail().x);
        bool        first = true;
        for (const BodyPart& bp : BodyParts()) {
            if (bp.id == "other") continue;
            const std::string label = bp.label + "##kit" + bp.id;
            const float       cw = ui::TextWidth(label.c_str()) + ui::Dp(14.0f);
            if (!first && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + cw <= x0 + wrap) ImGui::SameLine();
            else if (!first) ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y));
            first = false;
            const bool on = KitServes(*lib, bp.id);
            if (ui::ChipButton(label.c_str(), ImVec2(0.0f, 0.0f), on)) ToggleLibraryKitPart(uid, bp.id);
            ui::ItemTipText(on ? "Click to remove it from the kit" : "Click to add it to the kit");
        }
        // Played by: where it resolves now (Project and characters).
        const std::string played = AssignPlayedByText(uid);
        ui::LabelRow("Played by", ui::Dp(120.0f));
        ui::ToneText(played.empty() ? std::string("Not assigned yet") : played, played.empty() ? ui::kFaint : ui::kText, true);
        {
            ui::FontScope fs(ui::FontSmall());
            ui::ToneText("Kit = the body parts this library offers to serve (as Kit... in its menu). Played by = where it is "
                         "assigned now.",
                         ui::kMuted, true);
        }
    }
    ui::EndSection(sec);
}

void DrawColumn(int i)
{
    switch (i) {
    case 0:
        // 13-7a (audit C1.1 / C1.4): the collapse button sits in the Libraries header, and the user
        // libraries are listed in Libraries under USER (no User Library section of their own).
        DrawLibrariesSection();
        DrawSwitchGroupsSection();
        break;
    case 1: DrawLibraryTree(); break;
    case 2:
        SettleNodePanelFaders();  // a fader typed into before a click on the tree ended it
        if (SelectedLibraryRows().size() > 1) {
            DrawLibraryBatchPanel();
        } else {
            DrawNodePanel();
            DrawUserLibraryNodeSection();
            DrawLibraryRolesSection();
        }
        break;
    default:
        DrawMediaExplorerPanel();  // its card header carries the collapse button (13-7c)
        break;
    }
}

// 13-14 (C8): a collapsed column, the mockup's .colstrip: the whole strip is one button (kRaised
// under the mouse, a click expands it), 8 px from its top the 12 px muted chevron, then (column 1)
// the library's dot, then the name turned a quarter in 12 px kText, ellipsized to the strip.
void DrawStrip(int i, float h)
{
    ImGui::PushID(i);
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    ImGui::SetCursorScreenPos(wp);
    // The pane's padding insets its clip rect: open it to the whole strip, so its edge takes the mouse too.
    ImGui::PushClipRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), false);
    const bool clicked = ImGui::InvisibleButton("##expand", ImVec2(ws.x, std::max(1.0f, h)));
    const bool hot = ImGui::IsItemHovered();
    ImGui::PopClipRect();
    ui::ItemTipText(i == 0 ? "Show the libraries (Ctrl+B)" : "Show the Media Explorer");
    if (clicked) {
        if (i == 0) SetLibraryColumn1Collapsed(false);
        else {
            g_view.media_collapsed = false;
            SavePrefBool("lib_media_collapsed", false);
        }
    }
    std::string name = "Media";
    ImU32 dot = ui::kAccent;
    if (i == 0) {
        name.clear();
        if (const SoundLibrary* lib = FindLibrary(GetSoundStore(nullptr), SelectedLibraryUid())) {
            name = lib->name;
            if (lib->color & 0x1000000u)
                dot = IM_COL32((lib->color >> 16) & 0xFF, (lib->color >> 8) & 0xFF, lib->color & 0xFF, 0xFF);
        }
    }
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImVec2 b(wp.x + ws.x, wp.y + ws.y);
    dl->PushClipRect(wp, b, false);  // the whole strip (the pane's padding is not for its look)
    if (hot) dl->AddRectFilled(ImVec2(wp.x + 1.0f, wp.y + 1.0f), ImVec2(b.x - 1.0f, b.y - 1.0f), ui::kRaised,
                               std::max(0.0f, ui::Dp(ui::kRadiusLg) - 1.0f));
    ImFont*     f = ui::FontBody();
    const float cx = std::floor(wp.x + ws.x * 0.5f), gap = ui::Dp(8.0f);
    float       y = wp.y + gap;
    const char* chev = i == 0 ? "\xC2\xBB" : "\xC2\xAB";
    const ImVec2 cs = ui::TextSizeIn(f, chev);
    ui::AddTextIn(dl, f, ImVec2(std::floor(cx - cs.x * 0.5f), y), hot ? ui::kText : ui::kMuted, chev);
    y += cs.y + gap;
    if (i == 0) {
        const float r = ui::Dp(4.0f);
        dl->AddCircleFilled(ImVec2(cx, y + r), r, dot);
        y += 2.0f * r + gap;
    }
    const std::string shown = ui::EllipsizeToWidth(f, name, b.y - gap - y);
    ui::VerticalText(dl, f, cx, y, ui::kText, shown, i == 0, wp, b);
    dl->PopClipRect();
    ImGui::PopID();
}

// The splitter between columns a and b (ui::SplitterBar: accent-line on hover, accent while
// dragged), drag to resize, double-click resets.
void Splitter(int a, int b, float* px, float h, float visible_px, float visible_share)
{
    ImGui::PushID(a * 8 + b);
    const ui::SplitterState sp = ui::SplitterBar("##split", ImVec2(kColumnGap, std::max(1.0f, h)));
    if (sp.double_clicked) {
        ResetWidths();
    } else if (sp.active) {
        const float dx = sp.delta;
        if (dx != 0.0f) {
            float na = px[a] + dx, nb = px[b] - dx;
            if (na < kColumnMin[a]) {
                nb -= kColumnMin[a] - na;
                na = kColumnMin[a];
            }
            if (nb < kColumnMin[b]) {
                na -= kColumnMin[b] - nb;
                nb = kColumnMin[b];
            }
            if (na >= kColumnMin[a] && nb >= kColumnMin[b] && visible_px > 1.0f) {
                g_view.share[a] = na / visible_px * visible_share;
                g_view.share[b] = nb / visible_px * visible_share;
                px[a] = na;
                px[b] = nb;
            }
        }
    }
    if (sp.released) SaveWidths();
    ImGui::PopID();
}

void DrawToast()
{
    if (g_toast.text.empty()) return;
    if (ImGui::GetTime() > g_toast.until) {
        g_toast.text.clear();
        return;
    }
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    // 13-7 (audit B15): the mockup's .toast (pad 14 8, 13 px), its Undo a link.
    if (ui::BeginToast("##libtoast", ImVec2(ds.x * 0.5f, ds.y - ui::Dp(24.0f)))) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(g_toast.text.c_str());
        // Any later change (another edit, an undo) retires the Undo: it would undo that instead.
        if (g_toast.undo) {
            const int state = GetProjectStateChangeCount(nullptr);
            if (g_toast.state < 0) g_toast.state = state;
            g_toast.undo = GestureUndoAvailable(Undo_CanUndo2(nullptr), g_toast.undo_desc, state, g_toast.state);
        }
        if (g_toast.undo) {
            ImGui::SameLine(0.0f, ui::Dp(10.0f));
            if (ui::LinkButton("Undo")) {
                Main_OnCommand(40029, 0);  // Edit: Undo
                g_toast.text.clear();
            }
        }
        if (g_toast.local_undo) {
            ImGui::SameLine(0.0f, ui::Dp(10.0f));
            if (ui::LinkButton("Undo")) {
                const std::function<void()> undo = g_toast.local_undo;
                g_toast.local_undo = nullptr;
                g_toast.text.clear();
                undo();
            }
        }
    }
    ui::EndToast();
}

// 13-15c: a library with a kit that the project has not been offered yet, while at least one of its parts is
// free at the Project level: "<Library> serves <Parts>: use it as the project default?" [Use] [Not now]. Either
// way it is offered once (kit_offered is set in the same undo point).
void DrawKitOffer(float top)
{
    const SoundStore& store = GetSoundStore(nullptr);
    // Review 13-15c: offered only when a library is added to the project (imported, from the User Library):
    // the libraries the project had when this session first saw it, or that had nothing free when they came,
    // are never offered later out of context (a part cleared, an undo).
    // Keyed by the project and its file: REAPER keeps the same pointer when a tab opens another project.
    static std::map<std::string, std::set<std::string>> known;
    char        path[1024] = {};
    ReaProject* proj = EnumProjects(-1, path, static_cast<int>(sizeof(path)));
    char        ptr[32];
    std::snprintf(ptr, sizeof(ptr), "%p|", static_cast<void*>(proj));
    const std::string key = std::string(ptr) + path;
    auto              seen = known.find(key);
    if (seen == known.end()) {
        std::set<std::string>& k = known[key];
        for (const SoundLibrary& l : store.libraries) k.insert(l.uid);
        return;
    }
    const SoundLibrary* lib = nullptr;
    std::vector<std::string> free_parts;
    for (const SoundLibrary& l : store.libraries) {
        if (seen->second.count(l.uid)) continue;
        free_parts = assign::KitOfferParts(store, l);
        if (free_parts.empty()) {
            seen->second.insert(l.uid);  // nothing to offer when it came: never later
            continue;
        }
        lib = &l;
        break;
    }
    if (!lib) return;
    std::string parts;
    for (size_t i = 0; i < free_parts.size(); ++i)
        parts += (i == 0 ? "" : i + 1 == free_parts.size() ? " and " : ", ") + PartLabel(free_parts[i]);
    const std::string uid = lib->uid;
    const std::string text = lib->name + " serves " + parts + ": use it as the project default?";
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    if (ui::BeginToast("##libkitoffer", ImVec2(ds.x * 0.5f, top + ui::Dp(8.0f)), ImVec2(0.5f, 0.0f))) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text.c_str());
        ImGui::SameLine(0.0f, ui::Dp(10.0f));
        const bool use = ui::PrimaryButton("Use##kitoffer");
        ImGui::SameLine();
        const bool later = ui::SolidButton("Not now##kitoffer");
        if (use || later) {
            ui::EndToast();
            seen->second.insert(uid);
            OfferKit(uid, use);  // (the store is replaced: nothing above is read again)
            return;
        }
    }
    ui::EndToast();
}

}  // namespace

void ShowLibraryToast(const std::string& text, bool undo, const std::string& undo_desc)
{
    g_toast.text = text;
    g_toast.undo = undo;
    g_toast.undo_desc.clear();
    g_toast.local_undo = nullptr;
    if (undo) {
        const char* last = Undo_CanUndo2(nullptr);
        g_toast.undo_desc = !undo_desc.empty() ? undo_desc : (last ? last : "");
    }
    // Every caller shows its toast once the gesture's undo point is closed: read the state now,
    // so an undo made while the view is hidden cannot slip in before the first draw.
    g_toast.state = undo ? GetProjectStateChangeCount(nullptr) : -1;
    g_toast.until = ImGui::GetTime() + kToastSeconds;
}

void ShowLibraryToastLocal(const std::string& text, std::function<void()> undo)
{
    ShowLibraryToast(text, false);
    g_toast.local_undo = std::move(undo);
}

bool LibraryViewFocused() { return g_view.focused; }

bool LibraryViewTakesKey(unsigned vk, bool ctrl, bool shift, bool alt)
{
    if (LibraryNodeTakesKey(vk, ctrl, shift, alt)) return true;  // 13-7b: E (and the Edit overlay's keys; 13-63: S is free)
    // 13-48: the claim itself is pure (library_view_keys.h, host-tested).
    LibraryKeyState st;
    st.focused = g_view.focused;
    st.media_takes_arrows = st.focused && !g_view.media_collapsed && MediaPanelTakesArrows();  // 13-7c: Up/Down
    st.tree_takes_arrows = st.focused && LibraryTreeTakesArrows();                             // 13-17d: the arrows
    return LibraryViewClaimsKey(st, vk, ctrl, shift, alt);
}

bool LibraryColumn1Collapsed()
{
    LoadPrefs();
    return g_view.col1_collapsed;
}

void SetLibraryColumn1Collapsed(bool collapsed)
{
    LoadPrefs();
    if (g_view.col1_collapsed == collapsed) return;
    g_view.col1_collapsed = collapsed;
    SavePrefBool("lib_col1_collapsed", collapsed);
}

void SetLibraryMediaCollapsed(bool collapsed)
{
    LoadPrefs();
    if (g_view.media_collapsed == collapsed) return;
    g_view.media_collapsed = collapsed;
    SavePrefBool("lib_media_collapsed", collapsed);
}

void DrawLibraryView(float x, float y, float w, float h)
{
    RAV_QUIET_SCOPE("Library view", nullptr);  // freeze audit
    LoadPrefs();
    // The 3D view stays hidden behind the Library view (its gaps and margins included).
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), ImGui::GetIO().DisplaySize, ui::kBg);
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(std::max(1.0f, w), std::max(1.0f, h)), ImGuiCond_Always);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_HorizontalScrollbar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("##libraryview", nullptr, kFlags)) {
        g_view.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const ImGuiIO& io = ImGui::GetIO();
        if (g_view.focused && !io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_B, false))
            SetLibraryColumn1Collapsed(!g_view.col1_collapsed);

        // Widths: collapsed columns take a strip; the others share the rest by their shares.
        float strips = 0.0f, visible_share = 0.0f;
        for (int i = 0; i < 4; ++i) {
            if (Collapsed(i)) strips += kStripWidth;
            else visible_share += g_view.share[i];
        }
        const float visible_px = std::max(1.0f, w - 3.0f * static_cast<float>(kColumnGap) - strips);
        float px[4];
        for (int i = 0; i < 4; ++i)
            px[i] = Collapsed(i) ? static_cast<float>(kStripWidth)
                                 : std::max(static_cast<float>(kColumnMin[i]),
                                            std::floor(visible_px * g_view.share[i] / std::max(0.01f, visible_share)));

        static const char* const kIds[4] = {"##libcol1", "##libcol2", "##libcol3", "##libcol4"};
        const float ch = std::max(1.0f, h - 2.0f);
        for (int i = 0; i < 4; ++i) {
            if (i > 0) {
                ImGui::SameLine(0.0f, 0.0f);
                if (!Collapsed(i - 1) && !Collapsed(i)) Splitter(i - 1, i, px, ch, visible_px, visible_share);
                else ImGui::Dummy(ImVec2(kColumnGap, ch));
                ImGui::SameLine(0.0f, 0.0f);
            }
            // 13-7 (audit A2 / A3) / 13-14 (G-19): the mockup's .pane -- kSurface, a stroke edge, r10, 8 px
            // inside (ui::BeginPane); a collapsed strip draws its own content over the whole pane.
            const bool shown = ui::BeginPane(kIds[i], ImVec2(px[i], ch), ui::kSurface, 0,
                                             Collapsed(i) ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : 0);
            if (shown) {
                if (Collapsed(i)) DrawStrip(i, ch);
                else DrawColumn(i);
            }
            ui::EndPane();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    DrawToast();
    DrawKitOffer(y);
}

}  // namespace rav

#endif  // _WIN32
