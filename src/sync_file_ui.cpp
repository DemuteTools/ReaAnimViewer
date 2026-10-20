// SPDX-License-Identifier: MIT
//
// See sync_file_ui.h (story 13-77c).

#include "sync_file_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

#include "float_ui_window.h"
#include "library_icons.h"      // DrawNodeTypeGlyph, ToolIcon
#include "library_sync.h"
#include "library_sync_select.h"
#include "library_tree_edit.h"  // RowOfNode, LibraryLinked
#include "library_tree_ui.h"    // ShowInLibrary
#include "library_user_ui.h"    // LibrarySyncDiff, LibrarySyncInfoOf, RequestSyncApply
#include "library_view_ui.h"    // the toast repeated here
#include "sound_project.h"      // GetSoundStore
#include "sound_record.h"       // FindLibrary, FindNode, FindGroup
#include "ui_flow.h"
#include "ui_theme.h"

namespace rav {

namespace {

const char kDot[] = " \xC2\xB7 ";
const char kUpArrow[] = "\xE2\x86\x91";
const char kDownArrow[] = "\xE2\x86\x93";

// The mockup's 640 px window, its size kept, it opens at the mouse.
constexpr int kDefaultW = 760, kDefaultH = 560;
constexpr int kMinW = 460, kMinH = 300;
FloatUiWindow g_host("syncfile.float", L"Sync with file", kDefaultW, kDefaultH, kMinW, kMinH);

double ClockSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct State {
    bool                               open = false;
    bool                               focus = false;
    bool                               place = false;
    std::string                        lib;
    std::string                        scroll_to;
    SyncSelection                      sel;
    std::map<std::string, bool>        fold;  // a branch's own fold (absent = open when it has changed child branches)
    uint64_t                           sig = 0;
    bool                               have_sig = false;
    bool                               applying = false;
    double                             applying_until = 0.0;  // L1: an Apply that never answers stops blocking the button
    uint64_t                           epoch = 0;             // L2: moves each time the window opens fresh / on another library
    std::map<std::string, std::vector<std::string>> nb_cache;  // H1: "needed by" per key, per selection change
    uint64_t                           nb_ver = ~0ull, nb_sig = 0;
    std::map<std::string, std::vector<std::string>> under_memo, at_memo;  // H1: the keys under / at a branch, per diff
    bool                               post_apply = false;  // an Apply ended: when the list changes, ticks go, and it closes if empty
    double                             post_deadline = 0.0;
    std::vector<std::function<void()>> picks;   // gestures that leave this window (run after the frame)
    std::vector<std::function<void()>> defer;   // selection gestures (run after the body, before the footer)
    uint64_t                           toast_seen = 0;
    std::string                        toast;
    double                             toast_until = 0.0;
};
State g;

void Pick(std::function<void()> f)
{
    g.picks.push_back(std::move(f));
}

void Defer(std::function<void()> f)
{
    g.defer.push_back(std::move(f));
}

void SetToast(const std::string& t)
{
    if (t.empty()) return;
    g.toast = t;
    g.toast_until = ClockSeconds() + 5.0;
}

std::string When(int64_t t)
{
    if (t <= 0) return std::string();
    const std::time_t tt = static_cast<std::time_t>(t);
    std::tm           tmv{};
    localtime_s(&tmv, &tt);
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tmv);
    return buf;
}

uint64_t SigOf(const SyncDiff& d)
{
    return SyncDiffSignature(d);
}

// The keys under / at a branch, worked out once per diff (a frame asks for every open branch, many times).
const std::vector<std::string>& KeysUnder(const SyncDiff& d, const std::string& id)
{
    auto it = g.under_memo.find(id);
    if (it == g.under_memo.end()) it = g.under_memo.emplace(id, SyncKeysUnder(d, id)).first;
    return it->second;
}

const std::vector<std::string>& KeysAt(const SyncDiff& d, const std::string& id)
{
    auto it = g.at_memo.find(id);
    if (it == g.at_memo.end()) it = g.at_memo.emplace(id, SyncKeysAt(d, id)).first;
    return it->second;
}

// ---- what a frame draws from ---------------------------------------------------------------------------------------

struct Ctx {
    const SyncDiff*     d = nullptr;
    const SoundStore*   store = nullptr;
    const SoundLibrary* lib = nullptr;
    std::set<std::string>                            nodes;
    std::map<std::string, std::vector<std::string>> kids;  // changed child branches, sorted by label
    std::map<std::string, std::string>               label;
};

std::string Capital(const char* w)
{
    std::string s = w ? w : "";
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

std::string LabelOf(const Ctx& c, const std::string& id)
{
    if (id == "grp") return "Switch Groups";
    if (id.compare(0, 4, "grp:") == 0) {
        if (const SwitchGroup* sg = FindGroup(*c.store, id.substr(4))) return sg->name;
        const std::string n = SyncGhostName(*c.d, id);
        return n.empty() ? id.substr(4) : n;
    }
    if (const SoundNode* n = FindNode(*c.lib, id)) {
        if (!n->name.empty()) return n->name;
        return n->parent.empty() ? c.lib->name : Capital(SoundNodeTypeWord(n->type));
    }
    const std::string g = SyncGhostName(*c.d, id);
    return g.empty() ? id : g;
}

bool TypeOf(const Ctx& c, const std::string& id, SoundNodeType* out)
{
    if (const SoundNode* n = FindNode(*c.lib, id)) {
        *out = n->type;
        return true;
    }
    const std::string w = SyncGhostType(*c.d, id);
    if (w == "Blend") *out = SoundNodeType::Blend;
    else if (w == "Switch") *out = SoundNodeType::Switch;
    else if (w == "Sound") *out = SoundNodeType::Sound;
    else *out = SoundNodeType::Random;
    return true;
}

std::string LowerOf(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return s;
}

void BuildCtx(Ctx* c)
{
    for (const std::string& u : SyncChangedNodes(*c->d)) {
        c->nodes.insert(u);
        c->label[u] = LabelOf(*c, u);
    }
    for (const std::string& u : c->nodes) {
        const std::string p = SyncParentOf(*c->d, u);
        if (!p.empty() && c->nodes.count(p)) c->kids[p].push_back(u);
    }
    for (auto& kv : c->kids)
        std::stable_sort(kv.second.begin(), kv.second.end(),
                         [&](const std::string& a, const std::string& b) { return LowerOf(c->label[a]) < LowerOf(c->label[b]); });
}

bool IsOpen(const Ctx& c, const std::string& id)
{
    const auto it = g.fold.find(id);
    if (it != g.fold.end()) return it->second;
    const auto k = c.kids.find(id);
    return k != c.kids.end() && !k->second.empty();
}

void OpenWithAncestors(const Ctx& c, const std::string& id)
{
    for (std::string u = id; !u.empty(); u = SyncParentOf(*c.d, u)) g.fold[u] = true;
}

// ---- small drawing helpers ----------------------------------------------------------------------------------------

float RowH()
{
    return ui::ControlH() + ui::Dp(6.0f);
}

float Indent()
{
    return ui::Dp(16.0f);
}

bool HoverRect(ImVec2 a, ImVec2 b)
{
    return ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a, b, false);
}

void TipRect(ImVec2 a, ImVec2 b, const std::string& tip)
{
    if (!tip.empty() && HoverRect(a, b)) ui::ShowTip(tip.c_str());
}

struct SideLook {
    const char* text;
    const char* tip;
    ImU32       ink, line, fill;
};

SideLook LookOf(SyncSide s)
{
    if (s == SyncSide::Here)
        return {"edited here", "Changed here, not in the file yet", ui::kAccentInk, ui::kAccentLine, ui::kAccentSoft};
    if (s == SyncSide::File)
        return {"in file", "Changed in the file since this project last synced", ui::kWarn, ui::kWarnLine, ui::kWarnSoft};
    return {"both", "Changed here and in the file: you pick the side", ui::kBoth, ui::kBothLine, ui::kBothSoft};
}

// The side of a set of changes: both when one is, or when both sides hold some.
SyncSide SideOf(const SyncDiff& d, const std::vector<std::string>& keys)
{
    bool here = false, file = false, both = false;
    for (const std::string& k : keys) {
        const SyncChange* c = d.Find(k);
        if (!c) continue;
        here |= c->side == SyncSide::Here;
        file |= c->side == SyncSide::File;
        both |= c->side == SyncSide::Both;
    }
    return both || (here && file) ? SyncSide::Both : here ? SyncSide::Here : SyncSide::File;
}

float SideTagW(SyncSide s)
{
    return ui::SmallTagWidth(LookOf(s).text);
}

void DrawSideTag(ImDrawList* dl, ImVec2 p, SyncSide s)
{
    const SideLook l = LookOf(s);
    ui::DrawSmallTagTone(dl, p, l.text, l.ink, l.line, l.fill);
    TipRect(p, ImVec2(p.x + ui::SmallTagWidth(l.text), p.y + ui::SmallTagHeight()), l.tip);
}

// A tri-state box at (x, row top) centred on the row; true when clicked.
bool Box(const char* id, float x, float y, float rowh, SyncTri t)
{
    ImGui::SetCursorScreenPos(ImVec2(x, std::floor(y + (rowh - std::max(ImGui::GetFrameHeight(), ui::Dp(14.0f))) * 0.5f)));
    const bool off = t == SyncTri::Disabled;
    if (off) ImGui::BeginDisabled();
    const bool clicked = ui::TriCheckbox(id, t == SyncTri::All ? 1 : t == SyncTri::Some ? 2 : 0);
    if (off) ImGui::EndDisabled();
    return clicked && !off;
}

// ---- the rows -----------------------------------------------------------------------------------------------------

// Folds open on what a gesture ticked for the user (so the rows that say why are visible).
void OpenTicked(const SyncDiff& d, const SyncEffect& fx)
{
    for (const std::string& t : fx.ticked)
        if (const SyncChange* x = d.Find(t))
            for (std::string u = x->node; !u.empty(); u = SyncParentOf(d, u)) g.fold[u] = true;
}

void GroupDirGesture(const std::vector<std::string>& keys, SyncDir dir)
{
    const SyncDiff* d = LibrarySyncDiff(g.lib);
    if (!d) return;
    const SyncEffect fx = g.sel.GroupDir(*d, keys, dir);
    OpenTicked(*d, fx);
    SetToast(SyncEffectToast(*d, fx));
}

// Direction buttons of a parent row: right-aligned. Returns their left edge.
float DrawGroupDirs(const Ctx& c, const std::string& id, bool all, float x_right, float y, float rowh)
{
    const std::vector<std::string>& keys = KeysUnder(*c.d, all ? "*" : id);
    int                            nu = 0, nd = 0;
    SyncDirCounts(*c.d, keys, &nu, &nd);
    const std::string up = std::string(kUpArrow) + " mine " + std::to_string(nu);
    const std::string dn = std::string(kDownArrow) + " file's " + std::to_string(nd);
    const float       uw = ui::ButtonWidth(up.c_str()), dw = ui::ButtonWidth(dn.c_str());
    const float       gap = ui::Dp(4.0f);
    const float       by = std::floor(y + (rowh - ui::ControlH()) * 0.5f);
    const float       xd = x_right - dw, xu = xd - gap - uw;
    ImGui::SetCursorScreenPos(ImVec2(xu, by));
    if (nu == 0) ImGui::BeginDisabled();
    if (ui::SolidButton(up.c_str(), ImVec2(uw, 0.0f))) Defer([keys] { GroupDirGesture(keys, SyncDir::Up); });
    if (nu == 0) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ui::ShowTip("Tick, and send mine, for what changed here (and pick mine where both changed)");
    ImGui::SetCursorScreenPos(ImVec2(xd, by));
    if (nd == 0) ImGui::BeginDisabled();
    if (ui::SolidButton(dn.c_str(), ImVec2(dw, 0.0f))) Defer([keys] { GroupDirGesture(keys, SyncDir::Down); });
    if (nd == 0) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ui::ShowTip("Tick, and take the file's, for what changed in the file (and pick the file's where both changed)");
    return xu;
}

void ToggleGroupGesture(const std::vector<std::string>& keys)
{
    Defer([keys] {
        const SyncDiff* d = LibrarySyncDiff(g.lib);
        if (!d) return;
        const SyncEffect fx = g.sel.ToggleGroup(*d, keys);
        OpenTicked(*d, fx);
        SetToast(SyncEffectToast(*d, fx));
    });
}

// "Sync all": the library's whole list.
void DrawAllRow(const Ctx& c)
{
    const SyncDiff&                d = *c.d;
    const std::vector<std::string>& keys = KeysUnder(d, "*");
    const float                    rowh = RowH();
    const ImVec2                   p = ImGui::GetCursorScreenPos();
    const float                    w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImDrawList*                    dl = ImGui::GetWindowDrawList();
    ImGui::PushID("##all");
    if (Box("##ck", p.x + ui::Dp(22.0f), p.y, rowh, g.sel.TriState(d, keys))) ToggleGroupGesture(keys);
    float       x = p.x + ui::Dp(22.0f) + ui::Dp(14.0f) + ui::Dp(8.0f);
    const float ty = std::floor(p.y + (rowh - ImGui::GetTextLineHeight()) * 0.5f);
    float       right = p.x + w - ui::Dp(4.0f);
    if (!keys.empty()) right = DrawGroupDirs(c, "*", true, right, p.y, rowh) - ui::Dp(8.0f);
    ui::AddTextIn(dl, ui::FontSemibold(), ImVec2(x, ty), ui::kText, "Sync all");
    x += ui::TextSizeIn(ui::FontSemibold(), "Sync all").x + ui::Dp(8.0f);
    if (!keys.empty() && right - x > ui::Dp(30.0f))
        ui::EllipsisTextTip(dl, ImVec2(x, ty), right, ui::kMuted, SyncChangesWord(static_cast<int>(keys.size())));
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, rowh));
    ImGui::PopID();
}

void DrawGroupRow(const Ctx& c, const std::string& id, int depth)
{
    const SyncDiff&                d = *c.d;
    const std::vector<std::string>& keys = KeysUnder(d, id);
    const float                    rowh = RowH();
    const ImVec2                   p = ImGui::GetCursorScreenPos();
    const float                    w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImDrawList*                    dl = ImGui::GetWindowDrawList();
    if (HoverRect(p, ImVec2(p.x + w, p.y + rowh))) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowh), ui::kHover, ui::kRadiusSm);
    if (g.scroll_to == id) {
        ImGui::SetCursorScreenPos(p);
        ImGui::Dummy(ImVec2(1.0f, rowh));
        ImGui::SetScrollHereY(0.25f);
        g.scroll_to.clear();
    }
    ImGui::PushID(id.c_str());
    float x = p.x + static_cast<float>(depth) * Indent();
    // chevron
    const bool  open = IsOpen(c, id);
    const float cs = ui::Dp(18.0f);
    ImGui::SetCursorScreenPos(ImVec2(x, std::floor(p.y + (rowh - cs) * 0.5f)));
    if (ui::Chevron("##chev", open, cs)) g.fold[id] = !open;
    x += cs + ui::Dp(4.0f);
    if (Box("##ck", x, p.y, rowh, g.sel.TriState(d, keys))) ToggleGroupGesture(keys);
    x += ui::Dp(14.0f) + ui::Dp(8.0f);
    // glyph
    const float gs = ui::Dp(14.0f);
    const ImVec2 gp(x, std::floor(p.y + (rowh - gs) * 0.5f));
    if (id == "grp" || id.compare(0, 4, "grp:") == 0) DrawLibraryGlyph(dl, gp, gs, kGlyphGroup, ui::kMuted);
    else {
        SoundNodeType t = SoundNodeType::Random;
        TypeOf(c, id, &t);
        DrawNodeTypeGlyph(dl, gp, gs, t, ui::kAccentInk);
    }
    x += gs + ui::Dp(6.0f);
    // the fixed right column: ↑ mine / ↓ file's
    float right = p.x + w - ui::Dp(4.0f);
    right = DrawGroupDirs(c, id, false, right, p.y, rowh) - ui::Dp(8.0f);
    // the middle: name, new tag, count, side tag
    const std::string& label = c.label.at(id);
    ImFont*            nf = ui::FontBody();
    const float        ty = std::floor(p.y + (rowh - nf->FontSize) * 0.5f);
    const float        gap = ui::Dp(6.0f);
    const SyncChange*  adds = nullptr;
    for (const std::string& k : KeysAt(d, id)) {
        const SyncChange* ch = d.Find(k);
        if (ch && ch->adds_container && ch->subject == id && ch->side != SyncSide::Both) adds = ch;
    }
    const std::string newt = adds ? (adds->side == SyncSide::Here ? "new here" : "new in file") : std::string();
    const std::string cnt = SyncChangesWord(static_cast<int>(keys.size()));
    const SyncSide    side = SideOf(d, keys);
    const float       new_w = newt.empty() ? 0.0f : ui::SmallTagWidth(newt.c_str()) + gap;
    const float       cnt_w = ui::TextSizeIn(ui::FontSmall(), cnt.c_str()).x + gap;
    const float       side_w = SideTagW(side) + gap;
    const float       name_min = std::min(ui::TextSizeIn(nf, label.c_str()).x, ui::Dp(90.0f));
    float             room = right - x;
    const bool        show_extra = room >= name_min + new_w + cnt_w + side_w;
    float             extra = show_extra ? new_w + cnt_w + side_w : 0.0f;
    if (!show_extra && room >= name_min + side_w) extra = side_w;  // keep the side tag before the count
    const bool        show_cnt = show_extra;
    const bool        show_side = extra > 0.0f;
    const float       name_room = std::max(ui::Dp(20.0f), room - extra);
    const std::string shown = ui::EllipsizeToWidth(nf, label, name_room);
    const float       nw = std::max(1.0f, std::min(name_room, ui::TextSizeIn(nf, shown.c_str()).x));
    const bool        clickable = c.d->mine_nodes.count(id) && id.compare(0, 3, "grp") != 0;
    bool              clicked = false;
    if (clickable) {
        ImGui::SetCursorScreenPos(ImVec2(x, ty));
        clicked = ImGui::InvisibleButton("##nm", ImVec2(nw, nf->FontSize + 2.0f));
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            const float uy = ty + nf->FontSize + ui::Dp(0.5f);
            dl->AddLine(ImVec2(x, uy), ImVec2(x + nw, uy), ui::kText);
            ui::ItemTipText(shown != label ? (label + "\nShow in the tree").c_str() : "Show in the tree");
        }
    } else if (shown != label) {
        TipRect(ImVec2(x, ty), ImVec2(x + nw, ty + nf->FontSize), label);
    }
    ui::AddTextIn(dl, nf, ImVec2(x, ty), ui::kText, shown.c_str());
    if (clicked) Pick([id] {
        const SoundLibrary* l = FindLibrary(GetSoundStore(nullptr), g.lib);
        if (l && FindNode(*l, id)) ShowInLibrary(g.lib, RowOfNode(*l, id));
    });
    float mx = x + nw + gap;
    const float th = ui::SmallTagHeight();
    if (show_cnt && !newt.empty()) {
        ui::DrawSmallTagTone(dl, ImVec2(mx, std::floor(p.y + (rowh - th) * 0.5f)), newt.c_str(), LookOf(adds->side).ink,
                             LookOf(adds->side).line, LookOf(adds->side).fill);
        mx += ui::SmallTagWidth(newt.c_str()) + gap;
    }
    if (show_cnt) {
        ui::AddTextIn(dl, ui::FontSmall(), ImVec2(mx, std::floor(p.y + (rowh - ui::FontSmall()->FontSize) * 0.5f)), ui::kMuted, cnt.c_str());
        mx += ui::TextSizeIn(ui::FontSmall(), cnt.c_str()).x + gap;
    }
    if (show_side) DrawSideTag(dl, ImVec2(mx, std::floor(p.y + (rowh - th) * 0.5f)), side);
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, rowh));
    ImGui::PopID();
}

const char* DirTip(const SyncChange& ch, SyncDir k)
{
    if (k == SyncDir::Up)
        return ch.side == SyncSide::File ? "Put this project's version back in the file (the file's change is dropped)"
                                         : "Write this change, as it is here, into the file";
    return ch.side == SyncSide::Here ? "Take the file's version back here (your change is dropped)" : "Take the file's change here";
}

void ChangeGesture(const std::string& key, bool choose, SyncDir dir)
{
    Defer([key, choose, dir] {
        const SyncDiff* d = LibrarySyncDiff(g.lib);
        if (!d) return;
        const SyncEffect fx = choose ? g.sel.Choose(*d, key, dir) : g.sel.Toggle(*d, key);
        OpenTicked(*d, fx);
        SetToast(SyncEffectToast(*d, fx));
    });
}

void DrawChangeRow(const Ctx& c, const std::string& key, int depth)
{
    const SyncDiff&   d = *c.d;
    const SyncChange* ch = d.Find(key);
    if (!ch) return;
    const SyncTick* tk = g.sel.Tick(key);
    const bool      on = g.sel.IsOn(key);
    const bool      both = ch->side == SyncSide::Both;
    const float     l1h = ui::FontBody()->FontSize + ui::Dp(2.0f), l2h = ui::FontSmall()->FontSize + ui::Dp(1.0f);
    const float     rowh = std::max(RowH(), (both ? l1h + l2h : l1h) + ui::Dp(8.0f));
    const ImVec2    p = ImGui::GetCursorScreenPos();
    const float     w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImDrawList*     dl = ImGui::GetWindowDrawList();
    if (!ImGui::IsRectVisible(p, ImVec2(p.x + w, p.y + rowh))) {  // H1: a row out of view only takes its room
        ImGui::Dummy(ImVec2(w, rowh));
        return;
    }
    if (HoverRect(p, ImVec2(p.x + w, p.y + rowh))) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowh), ui::kHover, ui::kRadiusSm);
    ImGui::PushID(key.c_str());
    float x = p.x + static_cast<float>(depth) * Indent() + ui::Dp(18.0f) + ui::Dp(4.0f);
    if (Box("##ck", x, p.y, rowh, on ? SyncTri::All : SyncTri::None)) ChangeGesture(key, false, SyncDir::Up);
    x += ui::Dp(14.0f) + ui::Dp(8.0f);
    // the fixed right column: ↑ Send mine / ↓ Get file's
    SyncDir     def = SyncDir::Up;
    const bool  has_def = SyncDefaultDir(*ch, &def);
    const char* labels[2] = {"\xE2\x86\x91 Send mine", "\xE2\x86\x93 Get file's"};
    const float bw[2] = {ui::ButtonWidth(labels[0]), ui::ButtonWidth(labels[1])};
    const float gap = ui::Dp(4.0f);
    float       right = p.x + w - ui::Dp(4.0f);
    const float by = std::floor(p.y + (rowh - ui::ControlH()) * 0.5f);
    for (int i = 1; i >= 0; --i) {
        const SyncDir k = i == 0 ? SyncDir::Up : SyncDir::Down;
        const bool    is_on = on && tk && tk->dir == k;
        ImGui::SetCursorScreenPos(ImVec2(right - bw[i], by));
        const bool hit = is_on ? ui::ToggleButton(labels[i], true, ImVec2(bw[i], 0.0f)) : ui::SolidButton(labels[i], ImVec2(bw[i], 0.0f));
        if (!on && has_def && def == k)  // the default direction, outlined
            dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ch->side == SyncSide::Here ? ui::kAccentLine : ui::kWarnLine, ui::kRadiusSm);
        else if (!on && !has_def)  // a both change: both buttons ask to be picked
            dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ui::kBothLine, ui::kRadiusSm);
        ui::ItemTipText(DirTip(*ch, k));
        if (hit) ChangeGesture(key, true, k);
        right -= bw[i] + gap;
    }
    right -= ui::Dp(4.0f);
    // the side tag
    const float th = ui::SmallTagHeight();
    const float side_w = SideTagW(ch->side);
    const bool  room_side = right - x > ui::Dp(150.0f) + side_w;
    if (room_side) {
        DrawSideTag(dl, ImVec2(right - side_w, std::floor(p.y + (rowh - th) * 0.5f)), ch->side);
        right -= side_w + ui::Dp(6.0f);
    }
    // the "needed by" / "goes with" chip
    std::string chip, chip_tip;
    const SyncTick* bt = on && tk && tk->auto_ ? g.sel.Tick(tk->by) : nullptr;
    if (bt && bt->on && bt->dir == tk->dir) {  // H2: the asker still goes the same way
        const SyncChange* by_c = d.Find(tk->by);
        chip = std::string(tk->needs_why ? "needed by " : "goes with ") + (by_c ? by_c->text : tk->by);
        chip_tip = tk->needs_why ? "Ticked for you: the change on its right needs this one"
                                 : "Ticked for you: it goes away with the change on its right";
    } else if (on) {
        const auto nbit = g.nb_cache.find(key);  // H1: computed once per selection change, not per row per frame
        if (nbit != g.nb_cache.end() && !nbit->second.empty()) {
            const std::vector<std::string>& nb = nbit->second;
            const SyncChange* first = d.Find(nb[0]);
            chip = "needed by " + (first ? first->text : nb[0]) + (nb.size() > 1 ? " +" + std::to_string(nb.size() - 1) : std::string());
            chip_tip = "Another ticked change needs this one";
        }
    }
    const float region = right - x;
    if (!chip.empty() && region > ui::Dp(220.0f)) {
        ImFont*     sf = ui::FontSmall();
        const float cap = std::min(region * 0.45f, ui::Dp(260.0f));
        const std::string shown = ui::EllipsizeToWidth(sf, chip, cap - ui::Dp(12.0f));
        const float cw = ui::TextSizeIn(sf, shown.c_str()).x + ui::Dp(12.0f);
        const ImVec2 a(right - cw, std::floor(p.y + (rowh - th) * 0.5f));
        dl->AddRectFilled(a, ImVec2(a.x + cw, a.y + th), ui::kAccentSoft, ui::Dp(4.0f));
        dl->AddRect(a, ImVec2(a.x + cw, a.y + th), ui::kAccentLine, ui::Dp(4.0f));
        ui::AddTextIn(dl, sf, ImVec2(a.x + ui::Dp(6.0f), std::floor(a.y + (th - sf->FontSize) * 0.5f)), ui::kAccentText, shown.c_str());
        TipRect(a, ImVec2(a.x + cw, a.y + th), shown != chip ? chip_tip + "\n" + chip : chip_tip);
        right -= cw + ui::Dp(6.0f);
    }
    // the text: one line, or label + "Here ... · File ..." for a both change
    const float tx0 = x, tx1 = std::max(x + ui::Dp(40.0f), right);
    const std::string name = both && !ch->label.empty() ? ch->label : ch->text;
    std::string       tip;
    if (!ch->needs.empty()) {
        tip = "Needs: ";
        for (size_t i = 0; i < ch->needs.size(); ++i) {
            const SyncChange* n = d.Find(ch->needs[i]);
            tip += std::string(i ? ", " : "") + (n ? n->text : ch->needs[i]);
        }
    }
    const float top = std::floor(p.y + (rowh - (both ? l1h + l2h : l1h)) * 0.5f);
    ui::EllipsisTextTip(dl, ImVec2(tx0, top), tx1, ui::kText, name, tip.empty() ? nullptr : tip.c_str());
    if (both) {
        ui::FontScope f(ui::FontSmall());
        ui::EllipsisTextTip(dl, ImVec2(tx0, top + l1h), tx1, ui::kMuted, "Here " + ch->here_text + std::string(kDot) + "File " + ch->file_text);
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, rowh));
    ImGui::PopID();
}

void DrawNode(const Ctx& c, const std::string& id, int depth)
{
    DrawGroupRow(c, id, depth);
    if (!IsOpen(c, id)) return;
    for (const std::string& k : KeysAt(*c.d, id)) DrawChangeRow(c, k, depth + 1);
    const auto it = c.kids.find(id);
    if (it != c.kids.end())
        for (const std::string& k : it->second) DrawNode(c, k, depth + 1);
}

// ---- the window ---------------------------------------------------------------------------------------------------

void DrawTitle(const SoundLibrary* lib)
{
    const float  h = ui::Dp(30.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float  close_w = ui::ControlH();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    g_host.SetDragArea(p.x - ui::Dp(8.0f), p.y - ui::Dp(8.0f), p.x + w - close_w - ui::Dp(4.0f), p.y + h,
                       ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
    const float ty = std::floor(p.y + (h - ImGui::GetTextLineHeight()) * 0.5f);
    const char* title = "Sync with file";
    float       x = p.x + ui::Dp(2.0f);
    ui::AddTextIn(dl, ui::FontSemibold(), ImVec2(x, ty), ui::kText, title);
    x += ui::TextSizeIn(ui::FontSemibold(), title).x;
    const std::string rest = lib ? std::string(kDot) + lib->name + " \xE2\x86\x94 " + lib->ravlib_path : std::string();
    ui::EllipsisTextTip(dl, ImVec2(x, ty), p.x + w - close_w - ui::Dp(8.0f), ui::kMuted, rest);
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - close_w, p.y + (h - close_w) * 0.5f));
    if (ui::IconOnlyButton("##sfclose", ToolIcon(kToolCross), "Close (Esc)")) g.open = false;
    dl->AddLine(ImVec2(p.x - ui::Dp(8.0f), p.y + h), ImVec2(p.x + w + ui::Dp(8.0f), p.y + h), ui::kStroke);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + ui::Dp(8.0f)));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

void DrawSub(const LibrarySyncInfo& info)
{
    std::string line1;
    if (!info.by.empty()) line1 = "File written by " + info.by + (info.at > 0 ? ", " + When(info.at) : std::string());
    const std::string synced = info.synced_at > 0 ? "this project last synced " + When(info.synced_at) : std::string("this project never synced");
    line1 += (line1.empty() ? std::string() : std::string(kDot)) + synced + ".";
    ui::ToneText(line1, ui::kMuted, true);
    ui::ToneText(std::string("Nothing is written until you click Apply. Tick what to sync (a whole library, a branch, a container or one change) "
                             "and pick a direction: ") + kUpArrow + " Send mine writes it into the file, " + kDownArrow + " Get file's takes it here.",
                 ui::kMuted, true);
}

void DrawFooter(const SyncDiff* d, float h)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(p.x - ui::Dp(8.0f), p.y), ImVec2(p.x + w + ui::Dp(8.0f), p.y), ui::kStroke);
    const SyncCountsTicked cnt = d ? g.sel.Counts(*d) : SyncCountsTicked();
    const int              left = d ? static_cast<int>(d->changes.size()) : 0;
    const std::string      apply = cnt.ticked > 0 ? "Apply " + std::to_string(cnt.ticked) : std::string("Apply");
    const float            aw = ui::ButtonWidth(apply.c_str()), cw = ui::ButtonWidth("Close");
    const float            by = std::floor(p.y + (h - ui::LargeH()) * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - aw, by));
    const bool can = d && cnt.ticked > 0 && !g.applying;
    if (!can) ImGui::BeginDisabled();
    if (ui::PrimaryButton(apply.c_str(), ImVec2(aw, ui::LargeH()))) Pick([] {
        const SyncDiff* dd = LibrarySyncDiff(g.lib);
        if (!dd || g.applying) return;
        const std::map<std::string, SyncDir> picks = g.sel.Picks(*dd);
        if (picks.empty()) return;
        g.applying = true;
        g.applying_until = ClockSeconds() + 20.0;
        const uint64_t epoch = g.epoch;
        RequestSyncApply(g.lib, picks, [epoch](SyncApplyOutcome o) {
            if (epoch != g.epoch) return;  // L2: the window was reopened (or is on another library) since: not its Apply
            g.applying = false;
            if (o == SyncApplyOutcome::Applied || o == SyncApplyOutcome::Partly) {
                g.post_apply = true;
                g.post_deadline = ClockSeconds() + 6.0;
            }
        }, SigOf(*dd));
    });
    if (!can) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ui::ShowTip("One undo point for everything ticked");
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - aw - ui::Dp(6.0f) - cw, by));
    if (ui::SolidButton("Close", ImVec2(cw, ui::LargeH()))) g.open = false;
    // the summary, wrapped in the room left of the buttons
    const float room = std::max(ui::Dp(60.0f), w - aw - cw - ui::Dp(18.0f));
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ui::Dp(6.0f)));
    const std::string text = d ? SyncFooterText(cnt, left) : std::string();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + room);
    ui::FontScope f(ui::FontSmall());
    ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(d && left == 0 ? ui::kOk : cnt.ticked > 0 ? ui::kText : ui::kMuted));
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, h));
}

void DrawToast()
{
    const uint64_t serial = LibraryToastSerial();
    if (serial != g.toast_seen) {  // an Apply's result (or any library toast) is repeated here
        g.toast_seen = serial;
        SetToast(LibraryToastText());
    }
    if (g.toast.empty() || ClockSeconds() > g.toast_until) return;
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    const float  maxw = std::max(ui::Dp(60.0f), ws.x - ui::Dp(32.0f));
    ImFont*      f = ui::FontBody();
    const ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, maxw - ui::Dp(24.0f), g.toast.c_str());
    const ImVec2 sz(ts.x + ui::Dp(24.0f), ts.y + ui::Dp(16.0f));
    const ImVec2 a(std::floor(wp.x + (ws.x - sz.x) * 0.5f), std::floor(wp.y + ws.y - sz.y - ui::Dp(64.0f)));
    ImDrawList*  dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + sz.x, a.y + sz.y), ui::kRaised, ui::kRadiusMd);
    dl->AddRect(a, ImVec2(a.x + sz.x, a.y + sz.y), ui::kStrokeStrong, ui::kRadiusMd);
    dl->AddText(f, f->FontSize, ImVec2(a.x + ui::Dp(12.0f), a.y + ui::Dp(8.0f)), ui::kText, g.toast.c_str(), nullptr, maxw - ui::Dp(24.0f));
}

void DrawContent()
{
    const bool popup_before = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const SoundStore&   store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, g.lib);
    if (!lib || !LibraryLinked(*lib)) {  // the library went (or was unlinked): nothing to sync any more
        g.open = false;
        return;
    }
    const SyncDiff*       d = LibrarySyncDiff(g.lib);
    const LibrarySyncInfo info = LibrarySyncInfoOf(g.lib);
    if (d) {  // the list was rebuilt: ticks stay by key; after an Apply the applied rows are gone and nothing is ticked
        const uint64_t sig = SigOf(*d);
        if (!g.have_sig || sig != g.sig) {
            g.have_sig = true;
            g.sig = sig;
            g.under_memo.clear();
            g.at_memo.clear();
            g.sel.Rekey(*d);
            if (g.post_apply) {
                g.post_apply = false;
                g.sel.Clear();
                if (d->changes.empty()) {
                    g.open = false;  // nothing left: the window closes
                    return;
                }
            }
        } else if (g.post_apply && ClockSeconds() > g.post_deadline) {
            g.post_apply = false;
            g.sel.Clear();
        }
    }
    if (g.applying && ClockSeconds() > g.applying_until) g.applying = false;  // L1: no answer came: the button comes back
    if (d && (g.nb_ver != g.sel.Version() || g.nb_sig != g.sig)) {
        g.nb_ver = g.sel.Version();
        g.nb_sig = g.sig;
        g.nb_cache = g.sel.NeededByAll(*d);
    }
    DrawTitle(lib);
    const float footer_h = ui::LargeH() + ui::Dp(16.0f);
    const float body_h = std::max(ui::Dp(60.0f), ImGui::GetContentRegionAvail().y - footer_h - ui::Dp(4.0f));
    if (ImGui::BeginChild("##sfbody", ImVec2(0.0f, body_h), ImGuiChildFlags_None)) {
        DrawSub(info);
        ImGui::Dummy(ImVec2(0.0f, ui::Dp(2.0f)));
        if (!d) {
            ui::ToneText(info.bad ? "Could not read the file just now: it is read again when REAPER comes back to the front."
                                  : "Reading the file...",
                         ui::kMuted, true);
        } else {
            Ctx c;
            c.d = d;
            c.store = &store;
            c.lib = lib;
            BuildCtx(&c);
            if (!c.d->changes.empty()) {
                ui::Card all;
                ui::BeginCard(all);
                DrawAllRow(c);
                ui::EndCard(all, ui::kSurface, ui::kStroke);
                ui::Card tree;
                ui::BeginCard(tree);
                for (const std::string& top : SyncTopNodes(*d, std::vector<std::string>(c.nodes.begin(), c.nodes.end()))) DrawNode(c, top, 0);
                ui::EndCard(tree, ui::kSurface, ui::kStroke);
            } else {
                ui::ToneText("Nothing changed on either side.", ui::kMuted, true);
            }
        }
    }
    ImGui::EndChild();
    std::vector<std::function<void()>> defer;
    defer.swap(g.defer);
    for (const auto& f : defer)
        if (f) f();
    DrawFooter(d, footer_h);
    DrawToast();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !popup_before) g.open = false;
}

}  // namespace

void OpenSyncWithFile(const std::string& lib_uid, const std::string& focus_node)
{
    const bool fresh = !g.open || g.lib != lib_uid;
    if (fresh) {
        ++g.epoch;
        g.under_memo.clear();
        g.at_memo.clear();
        g.nb_cache.clear();
        g.sel.Clear();
        g.fold.clear();
        g.have_sig = false;
        g.applying = false;
        g.post_apply = false;
        g.toast.clear();
        g.toast_seen = LibraryToastSerial();
        g.place = true;
    }
    g.open = true;
    g.focus = true;
    g.lib = lib_uid;
    g.scroll_to.clear();
    if (!focus_node.empty()) {
        if (const SyncDiff* d = LibrarySyncDiff(lib_uid)) {
            for (std::string u = focus_node; !u.empty(); u = SyncParentOf(*d, u)) g.fold[u] = true;
            g.scroll_to = focus_node;
        } else {
            g.fold[focus_node] = true;
            g.scroll_to = focus_node;
        }
    }
}

bool SyncWithFileIsOpen()
{
    return g.open;
}

void DrawSyncWithFileFloat(HWND viewer, HDC viewer_dc, HGLRC rc)
{
    if (!ImGui::GetCurrentContext()) return;
    if (!g.open && !g_host.IsShown()) return;
    g_host.Bind(viewer, viewer_dc, rc);
    if (g.place && g.open) g_host.PlaceAtMouse(static_cast<int>(std::lround(ui::Dp(8.0f + 15.0f))));
    g.place = false;
    const bool activate = g.open && g.focus;
    g.focus = false;
    g_host.Frame(g.open, activate, [] { DrawContent(); });
    if (g_host.TakeCloseRequest()) g.open = false;
    if (!g.open) {  // closed: the ticks are view state, dropped
        g.sel.Clear();
        g.picks.clear();
        g.defer.clear();
        g.have_sig = false;
        return;
    }
    std::vector<std::function<void()>> picks;
    picks.swap(g.picks);
    for (const auto& f : picks)
        if (f) f();
}

void HideSyncWithFileFloat()
{
    g_host.Hide();
}

void DestroySyncWithFileFloat()
{
    g_host.Destroy();
    g.open = false;
    g.sel.Clear();
    g.picks.clear();
    g.defer.clear();
}

bool SyncWithFileFloatOwns(HWND hwnd)
{
    const HWND h = g_host.Hwnd();
    return h && hwnd && (hwnd == h || IsChild(h, hwnd));
}

HWND SyncWithFileFloatHwnd()
{
    return g_host.Hwnd();
}

bool SyncWithFileFloatWantsText()
{
    return g_host.WantsText();
}

}  // namespace rav

#endif  // _WIN32
