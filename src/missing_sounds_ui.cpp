// SPDX-License-Identifier: MIT
//
// See missing_sounds_ui.h (spec missing-sounds-window).

#include "missing_sounds_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <imgui.h>

#include "float_ui_window.h"
#include "library_icons.h"     // ToolIcon
#include "library_node_ui.h"   // UseSelectedItemForSound
#include "library_tree_edit.h"  // MissingSound, MissingStatus, FixedStatus, RowOfNode
#include "library_tree_ui.h"   // ShowInLibrary
#include "library_user_ui.h"   // the one missing rule, Locate, Search a folder
#include "library_view_ui.h"   // the toast repeated here
#include "reaper_api.h"
#include "sound_audition.h"    // AuditionFile (a sound found by its name: listen to it)
#include "sound_populate.h"    // FindItemByGuid
#include "sound_project.h"     // GetSoundStore, SoundStoreGeneration
#include "sound_record.h"      // FindLibrary, FindNode
#include "ui_flow.h"
#include "ui_theme.h"

namespace rav {

namespace {

const char kDot[] = " \xC2\xB7 ";

// The mockup's 720 px window; its size is kept, it opens at the mouse. Narrower than kMinW the name column would
// have no room beside the fixed Library (120) and buttons (186) columns.
constexpr int kDefaultW = 720, kDefaultH = 420;
constexpr int kMinW = 440, kMinH = 220;
FloatUiWindow g_host("missingsounds.float", L"Missing sounds", kDefaultW, kDefaultH, kMinW, kMinH);

double ClockSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// One row: a sound missing now, or fixed since the window opened (it stays until the window closes).
struct Row {
    MissingSound m;               // as last seen missing
    bool         fixed = false;
    bool         by_name = false;  // fixed by a file found by its name only: "check it"
    std::string  plays;            // what a fixed row plays now (a file, or an item)
    std::string  path;             // a by-name row's file (Listen)
};

struct State {
    bool                               open = false;
    bool                               focus = false;
    bool                               place = false;
    std::string                        focus_lib;
    std::vector<Row>                   rows;
    std::string                        key;        // the store / project state the rows were read at
    double                             next_read = 0.0;
    std::vector<std::function<void()>> picks;      // run after the frame (one gesture each)
    uint64_t                           toast_seen = 0;
    std::string                        toast;
    double                             toast_until = 0.0;
};
State g;

void Pick(std::function<void()> f)
{
    g.picks.push_back(std::move(f));
}

// What a fixed sound plays now: its item (by its take's name) while the item is there, else its file.
std::string PlaysText(const SoundNode& n)
{
    if (n.sound.source.kind == SoundSourceKind::Item && !ItemSoundGone(n)) {
        std::string take;
        if (MediaItem* it = FindItemByGuid(n.sound.source.item_guid))
            if (MediaItem_Take* t = GetActiveTake(it))
                if (const char* nm = GetTakeName(t)) take = nm;
        return take.empty() ? std::string("its item") : "item \"" + take + "\"";
    }
    const std::string path = SoundNodeFilePath(n);
    return path.empty() ? n.sound.source.asset.last_path : path;
}

// The rows again: each sound missing now (added at the end when new), the others fixed (a sound deleted since
// leaves). Read when the store or the project changed, and once a second (a file moved outside REAPER).
void ReadRows()
{
    const std::string key = std::to_string(SoundStoreGeneration()) + "|" + std::to_string(GetProjectStateChangeCount(nullptr)) +
                            "|" + std::to_string(UserLibraryLocating() ? 1 : 0);
    const double now = ClockSeconds();
    if (key == g.key && now < g.next_read) return;
    g.key = key;
    g.next_read = now + 1.0;
    const std::vector<MissingSound> cur = ListAllMissingSounds();
    std::map<std::string, const MissingSound*> by_key;
    for (const MissingSound& m : cur) by_key[m.lib_uid + "\n" + m.uid] = &m;
    const SoundStore& store = GetSoundStore(nullptr);
    std::vector<Row>  kept;
    for (Row& r : g.rows) {
        const std::string k = r.m.lib_uid + "\n" + r.m.uid;
        const auto        hit = by_key.find(k);
        if (hit != by_key.end()) {
            r.m = *hit->second;
            r.fixed = r.by_name = false;
            r.plays.clear();
            by_key.erase(hit);
            kept.push_back(r);
            continue;
        }
        const SoundLibrary* lib = FindLibrary(store, r.m.lib_uid);
        const SoundNode*    n = lib ? FindNode(*lib, r.m.uid) : nullptr;
        if (!n || n->type != SoundNodeType::Sound) continue;  // deleted since
        r.fixed = true;
        r.by_name = SoundFoundByName(r.m.lib_uid, r.m.uid);
        r.plays = PlaysText(*n);
        r.path = SoundNodeFilePath(*n);
        if (!n->name.empty()) r.m.name = n->name;
        kept.push_back(r);
    }
    for (const MissingSound& m : cur)
        if (by_key.count(m.lib_uid + "\n" + m.uid)) {
            Row r;
            r.m = m;
            kept.push_back(r);
        }
    g.rows.swap(kept);
}

// The clicked library's rows first, then the others in library order, each library's in its node order.
std::vector<const Row*> SortedRows()
{
    const SoundStore&          store = GetSoundStore(nullptr);
    std::map<std::string, int> lib_at;
    std::map<std::string, int> node_at;
    for (size_t i = 0; i < store.libraries.size(); ++i) {
        const SoundLibrary& l = store.libraries[i];
        lib_at[l.uid] = static_cast<int>(i);
        for (size_t k = 0; k < l.nodes.size(); ++k) node_at[l.uid + "\n" + l.nodes[k].uid] = static_cast<int>(k);
    }
    std::vector<const Row*> out;
    for (const Row& r : g.rows) out.push_back(&r);
    auto rank = [&](const Row* r) {
        const int li = r->m.lib_uid == g.focus_lib ? -1 : (lib_at.count(r->m.lib_uid) ? lib_at[r->m.lib_uid] : 1 << 20);
        const auto ni = node_at.find(r->m.lib_uid + "\n" + r->m.uid);
        return std::make_pair(li, ni == node_at.end() ? 1 << 20 : ni->second);
    };
    std::stable_sort(out.begin(), out.end(), [&](const Row* a, const Row* b) { return rank(a) < rank(b); });
    return out;
}

// ---- The title row (the mockup's .fw-h) -------------------------------------------------------------------------

void DrawTitle(int left)
{
    const float  h = ui::Dp(30.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float  close_w = ui::ControlH();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    g_host.SetDragArea(p.x - ui::Dp(8.0f), p.y - ui::Dp(8.0f), p.x + w - close_w - ui::Dp(4.0f), p.y + h,
                       ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
    const float       ty = std::floor(p.y + (h - ImGui::GetTextLineHeight()) * 0.5f);
    const char*       title = "Missing sounds";
    float             x = p.x + ui::Dp(2.0f);
    ui::AddTextIn(dl, ui::FontSemibold(), ImVec2(x, ty), ui::kText, title);
    x += ui::TextSizeIn(ui::FontSemibold(), title).x;
    const std::string what = left > 0 ? std::to_string(left) + " to fix" : std::string("all fixed");
    ui::EllipsisTextTip(dl, ImVec2(x, ty), p.x + w - close_w - ui::Dp(8.0f), ui::kMuted, std::string(kDot) + what);
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - close_w, p.y + (h - close_w) * 0.5f));
    if (ui::IconOnlyButton("##msclose", ToolIcon(kToolCross), "Close (Esc)")) g.open = false;
    dl->AddLine(ImVec2(p.x - ui::Dp(8.0f), p.y + h), ImVec2(p.x + w + ui::Dp(8.0f), p.y + h), ui::kStroke);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + ui::Dp(8.0f)));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

// ---- Search a folder (the first card) ---------------------------------------------------------------------------

void DrawSearch(int left)
{
    ui::Card card;
    ui::BeginCard(card);
    const bool locating = UserLibraryLocating();
    if (locating || left == 0) ImGui::BeginDisabled();
    if (ui::PrimaryButton("Search a folder...")) Pick([] { RequestMissingSearchFolder(); });
    if (locating || left == 0) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ui::ShowTip(left == 0 ? "No missing sound in any library"
                              : "Pick the folder that holds the sounds (for example your sound library): RAV searches it "
                                "and its subfolders for every missing sound of every library (one undo step)");
    const std::string& last = MissingLastSearchedFolder();
    const std::string  hint = last.empty() ? std::string("RAV searches it and its subfolders for every missing sound")
                                           : "Last searched: " + last;
    ImGui::SameLine();
    {
        ui::FontScope f(ui::FontSmall());
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  room = std::max(ui::Dp(40.0f), ImGui::GetContentRegionAvail().x);
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + std::max(0.0f, (ui::ControlH() - ImGui::GetTextLineHeight()) * 0.5f)));
        ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::kFaint));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + room);
        ImGui::TextUnformatted(hint.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    if (locating) DrawLibraryLocateStatus();  // its Cancel
    ui::EndCard(card, ui::kSurface, ui::kStroke);
}

// ---- The table (the second card) --------------------------------------------------------------------------------

// The row's buttons, right-aligned in the fixed column: a missing sound's Locate... and Use selected item, a sound
// found by its name's Listen and Locate..., none for a fixed one.
void DrawRowButtons(const Row& r, float x_right, float cy)
{
    struct Btn {
        const char* label;
        float       w;
    };
    std::vector<Btn> btns;
    const bool       missing = !r.fixed;
    if (missing) {
        btns.push_back({"Locate...", ui::ButtonWidth("Locate...")});
        btns.push_back({"Use selected item", ui::ButtonWidth("Use selected item")});
    } else if (r.by_name) {
        btns.push_back({"\xE2\x96\xB6##listen", ui::ButtonWidth("\xE2\x96\xB6")});
        btns.push_back({"Locate...", ui::ButtonWidth("Locate...")});
    }
    if (btns.empty()) return;
    const float gap = ui::Dp(4.0f);
    const float col_w = ui::Dp(186.0f) - 2.0f * ui::Dp(6.0f);  // the column less its cell padding
    float       total = gap * static_cast<float>(btns.size() - 1);
    for (const Btn& b : btns) total += b.w;
    if (total > col_w) {  // the last (widest) one shrinks and ellipsizes; the column never moves
        btns.back().w = std::max(ui::Dp(24.0f), btns.back().w - (total - col_w));
        total = col_w;
    }
    float       x = x_right - total;
    const float y = std::floor(cy - ui::ControlH() * 0.5f);
    const bool  locating = UserLibraryLocating();
    const std::string lib = r.m.lib_uid, node = r.m.uid;
    for (const Btn& b : btns) {
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        const std::string label = b.label;
        if (label == "Locate...") {
            if (locating) ImGui::BeginDisabled();
            if (ui::SolidButton("Locate...", ImVec2(b.w, 0.0f))) Pick([lib, node] { RequestUserLibraryLocate(lib, node, true); });
            if (locating) ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ui::ShowTip(locating ? "Already locating: wait, or Cancel above"
                                     : "Pick this sound's file yourself: the other missing sounds are looked for in its folder");
        } else if (label == "Use selected item") {
            const bool any = CountSelectedMediaItems(nullptr) > 0;
            if (!any) ImGui::BeginDisabled();
            if (ui::SolidButton("Use selected item", ImVec2(b.w, 0.0f))) Pick([lib, node] { UseSelectedItemForSound(lib, node); });
            if (!any) ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ui::ShowTip(any ? "Use the item selected in REAPER: this sound then plays it"
                                : "Select an item in REAPER first: this sound then plays it");
        } else {
            const std::string path = r.path;
            if (path.empty()) ImGui::BeginDisabled();
            if (ui::SolidButton(b.label, ImVec2(b.w, 0.0f)) && !path.empty()) Pick([path] { AuditionFile(path, 0.0, 0.0); });
            if (path.empty()) ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ui::ShowTip(path.empty() ? "Its file is not found" : "Listen");
        }
        x += b.w + gap;
    }
}

void DrawTable(const std::vector<const Row*>& rows)
{
    if (rows.empty()) return;
    ui::Card card;
    ui::BeginCard(card);
    const SoundStore& store = GetSoundStore(nullptr);
    const ImVec2      pad(ui::Dp(6.0f), ui::Dp(5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, pad);
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ui::Col(ui::kStroke));
    const float  table_x0 = ImGui::GetCursorScreenPos().x;
    const float  table_w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX;
    if (ImGui::BeginTable("##missingtbl", 3, flags, ImVec2(table_w, 0.0f))) {
        ImGui::TableSetupColumn("Asset name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Library", ImGuiTableColumnFlags_WidthFixed, ui::Dp(120.0f) - 2.0f * pad.x);
        ImGui::TableSetupColumn("##acts", ImGuiTableColumnFlags_WidthFixed, ui::Dp(186.0f) - 2.0f * pad.x);
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        ImGui::TableSetColumnIndex(0);
        ui::GroupCaption("Asset name");
        ImGui::TableSetColumnIndex(1);
        ui::GroupCaption("Library");
        ImFont*     name_f = ui::FontBody();
        ImFont*     st_f = ui::FontSmall();
        const float l1 = name_f->FontSize + ui::Dp(2.0f), l2 = st_f->FontSize;
        const float content_h = std::max(l1 + l2, ui::ControlH());
        int         idx = 0;
        for (const Row* r : rows) {
            ImGui::PushID(idx++);
            ImGui::TableNextRow(0, content_h + 2.0f * pad.y);
            ImGui::TableSetColumnIndex(0);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float  y0 = p.y - pad.y, y1 = y0 + content_h + 2.0f * pad.y;
            ImDrawList*  dl = ImGui::GetWindowDrawList();
            if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ImVec2(table_x0, y0), ImVec2(table_x0 + table_w, y1), false))
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ui::kHover);
            if (r->m.lib_uid == g.focus_lib) {  // the clicked library's rows: the accent bar (inset 2 px)
                dl->PushClipRectFullScreen();
                dl->AddRectFilled(ImVec2(table_x0, y0), ImVec2(table_x0 + ui::Dp(2.0f), y1), ui::kAccent);
                dl->PopClipRect();
            }
            // Asset name (click: shown in the tree) and its status line.
            const float w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
            const float top = std::floor(p.y + (content_h - (l1 + l2)) * 0.5f);
            const std::string shown = ui::EllipsizeToWidth(name_f, r->m.name, w);
            const float       nw = std::max(1.0f, std::min(w, ui::TextSizeIn(name_f, shown.c_str()).x));
            ImGui::SetCursorScreenPos(ImVec2(p.x, top));
            const bool clicked = ImGui::InvisibleButton("##name", ImVec2(nw, l1));
            const bool hov = ImGui::IsItemHovered();
            ui::AddTextIn(dl, name_f, ImVec2(p.x, top), r->fixed ? ui::kMuted : ui::kText, shown.c_str());
            if (hov) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                const float uy = top + name_f->FontSize + ui::Dp(0.5f);
                dl->AddLine(ImVec2(p.x, uy), ImVec2(p.x + nw, uy), r->fixed ? ui::kMuted : ui::kText);
                ui::ItemTipText(shown != r->m.name ? (r->m.name + "\nShow in the tree").c_str() : "Show in the tree");
            }
            if (clicked) {
                const std::string lib = r->m.lib_uid, node = r->m.uid;
                Pick([lib, node] {
                    const SoundLibrary* l = FindLibrary(GetSoundStore(nullptr), lib);
                    if (l && FindNode(*l, node)) ShowInLibrary(lib, RowOfNode(*l, node));
                });
            }
            const std::string status = r->fixed ? FixedStatus(r->plays, r->by_name) : MissingStatus(r->m);
            const ImU32       st_col = !r->fixed ? ui::kMissText : r->by_name ? ui::kWarn : ui::kOk;
            {
                ui::FontScope f(st_f);
                ui::EllipsisTextTip(dl, ImVec2(p.x, top + l1), p.x + w, st_col, status);
            }
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + content_h));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            // Library: its dot and name.
            ImGui::TableSetColumnIndex(1);
            {
                const ImVec2        q = ImGui::GetCursorScreenPos();
                const float         lw = ImGui::GetContentRegionAvail().x;
                const SoundLibrary* l = FindLibrary(store, r->m.lib_uid);
                const float         cy = q.y + content_h * 0.5f;
                dl->AddCircleFilled(ImVec2(q.x + ui::Dp(4.0f), cy), ui::Dp(4.0f), ui::LibDot(l ? l->color : 0));
                ImFont* lf = ui::FontBody();
                ui::EllipsisTextTip(dl, ImVec2(q.x + ui::Dp(14.0f), std::floor(cy - lf->FontSize * 0.5f)), q.x + lw, ui::kMuted,
                                    l ? l->name : std::string("(library)"));
                ImGui::Dummy(ImVec2(1.0f, content_h));
            }
            // The buttons, in the fixed right column.
            ImGui::TableSetColumnIndex(2);
            {
                const ImVec2 q = ImGui::GetCursorScreenPos();
                const float  aw = ImGui::GetContentRegionAvail().x;
                DrawRowButtons(*r, q.x + aw, q.y + content_h * 0.5f);
                ImGui::SetCursorScreenPos(q);
                ImGui::Dummy(ImVec2(1.0f, content_h));
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ui::EndCard(card, ui::kSurface, ui::kStroke);
}

// The last toast (a gesture of this window, a search's result), repeated at the bottom of this window for ~5 s.
void DrawToast()
{
    const uint64_t serial = LibraryToastSerial();
    if (serial != g.toast_seen) {
        g.toast_seen = serial;
        g.toast = LibraryToastText();
        g.toast_until = ClockSeconds() + 5.0;
    }
    if (g.toast.empty() || ClockSeconds() > g.toast_until) return;
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    const float  maxw = std::max(ui::Dp(60.0f), ws.x - ui::Dp(32.0f));
    ImFont*      f = ui::FontBody();
    const ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, maxw - ui::Dp(24.0f), g.toast.c_str());
    const ImVec2 sz(ts.x + ui::Dp(24.0f), ts.y + ui::Dp(16.0f));
    const ImVec2 a(std::floor(wp.x + (ws.x - sz.x) * 0.5f), std::floor(wp.y + ws.y - sz.y - ui::Dp(18.0f)));
    ImDrawList*  dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + sz.x, a.y + sz.y), ui::kRaised, ui::kRadiusMd);
    dl->AddRect(a, ImVec2(a.x + sz.x, a.y + sz.y), ui::kStrokeStrong, ui::kRadiusMd);
    dl->AddText(f, f->FontSize, ImVec2(a.x + ui::Dp(12.0f), a.y + ui::Dp(8.0f)), ui::kText, g.toast.c_str(), nullptr,
                maxw - ui::Dp(24.0f));
}

void DrawContent()
{
    const bool popup_before = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    ReadRows();
    int left = 0;
    for (const Row& r : g.rows) left += r.fixed ? 0 : 1;
    DrawTitle(left);
    const float h = std::max(ui::Dp(60.0f), ImGui::GetContentRegionAvail().y);
    if (ImGui::BeginChild("##msbody", ImVec2(0.0f, h), ImGuiChildFlags_None)) {
        DrawSearch(left);
        if (left == 0) ui::ToneText("Every sound plays again.", ui::kOk);
        DrawTable(SortedRows());
    }
    ImGui::EndChild();
    DrawToast();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !popup_before) g.open = false;
}

}  // namespace

void OpenMissingSounds(const std::string& focus_lib)
{
    if (!g.open) {  // a fresh opening: the rows read again, nothing fixed yet
        g.rows.clear();
        ForgetFoundByName();
        g.toast_seen = LibraryToastSerial();  // an older toast is not repeated here
        g.toast.clear();
    }
    g.open = true;
    g.focus = true;
    g.place = true;
    g.focus_lib = focus_lib;
    g.key.clear();
}

bool MissingSoundsIsOpen()
{
    return g.open;
}

void DrawMissingSoundsFloat(HWND viewer, HDC viewer_dc, HGLRC rc)
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
    if (!g.open) {  // closed: the fixed rows go (spec: "until the window closes")
        g.rows.clear();
        g.picks.clear();
        ForgetFoundByName();
        return;
    }
    // Its gestures run now, the frame drawn and the viewer's contexts back (each one undo point of its own).
    std::vector<std::function<void()>> picks;
    picks.swap(g.picks);
    for (const auto& f : picks)
        if (f) f();
}

void HideMissingSoundsFloat()
{
    g_host.Hide();
}

void DestroyMissingSoundsFloat()
{
    g_host.Destroy();
    g.open = false;  // a viewer made again later does not bring it back unasked
    g.rows.clear();
    g.picks.clear();
}

bool MissingSoundsFloatOwns(HWND hwnd)
{
    const HWND h = g_host.Hwnd();
    return h && hwnd && (hwnd == h || IsChild(h, hwnd));
}

HWND MissingSoundsFloatHwnd()
{
    return g_host.Hwnd();
}

bool MissingSoundsFloatWantsText()
{
    return g_host.WantsText();
}

}  // namespace rav

#endif  // _WIN32
