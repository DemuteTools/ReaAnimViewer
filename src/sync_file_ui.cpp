// SPDX-License-Identifier: MIT
//
// See sync_file_ui.h (story 13-77c).

#include "sync_file_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <cctype>
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
const char kArrow[] = " \xE2\x80\xBA ";
const char kDash[] = "\xE2\x80\x94";

// Mockup v5: 960 px wide, a 24 px row per change; it opens at the mouse. (A new host id: the old layout's kept size is dropped.)
constexpr int kDefaultW = 960, kDefaultH = 640;
constexpr int kMinW = 460, kMinH = 300;
FloatUiWindow g_host("syncfile5.float", L"Sync with global", kDefaultW, kDefaultH, kMinW, kMinH);

double ClockSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- what the window draws: one row per change, computed once per diff (not per frame) --------------------------------

struct Row {
    std::string key;
    int         group = 0;
    SyncSide    side = SyncSide::Both;
    std::string par, last;     // the CHANGE column: a muted start, then the name
    std::string full_name;     // "par > last": the "needed by" chips
    std::string gtxt, ptxt;    // the two value cells (the global's / this project's), names with the common ends hidden
    std::string gfull, pfull;  // the same untrimmed: the tooltips and the detail strip
    size_t      glo = 0, ghi = 0, plo = 0, phi = 0;  // the differing part of each cell
    bool        gnone = false, pnone = false;        // nothing on that side: a dash
    bool        add = false;                         // an addition: one box, on the side that added it
    SyncSide    adder = SyncSide::Both;
    int         icon = -1;                           // a SoundNodeType, or 100 + glyph (-1 none)
    std::vector<std::string> tree;                   // the added branch's content (the detail strip)
};

struct Group {
    std::string              id;      // container uid, or "grp" (Switch Groups)
    std::vector<std::string> segs;    // the path: names from the top down
    std::string              hint;    // "by SurfaceMaterial"
    std::vector<int>         rows;
    std::vector<std::string> keys;
};

struct Model {
    std::vector<Row>           rows;
    std::vector<Group>         groups;
    std::map<std::string, int> row_of;
    std::vector<std::string>   all;  // every key, in diff order
    std::map<std::string, int> group_of;
};

struct State {
    bool                               open = false;
    bool                               focus = false;
    bool                               place = false;
    std::string                        lib;
    std::string                        scroll_to;
    SyncSelection                      sel;
    std::map<std::string, bool>        fold;  // a group's own fold (absent = open)
    std::string                        open_row;  // the change whose detail strip is open (one at a time)
    Model                              model;
    uint64_t                           sig = 0;
    bool                               have_sig = false;
    bool                               applying = false;
    double                             applying_until = 0.0;  // L1: an Apply that never answers stops blocking the button
    uint64_t                           epoch = 0;             // L2: moves each time the window opens fresh / on another library
    std::map<std::string, std::vector<std::string>> nb_cache;  // H1: "needed by" per key, per selection change
    uint64_t                           nb_ver = ~0ull, nb_sig = 0;
    std::string                        foot_text, foot_full;  // the footer summary, per selection change
    int                                foot_pick = 0;
    uint64_t                           foot_ver = ~0ull, foot_sig = 0;
    float                              body_sb = 0.0f;        // the body's scrollbar width last frame (the header lines up)
    bool                               post_apply = false;  // an Apply ended: when the list changes, the window closes if empty
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

// ---- names ---------------------------------------------------------------------------------------------------------

struct Ctx {
    const SyncDiff*     d = nullptr;
    const SoundStore*   store = nullptr;
    const SoundLibrary* lib = nullptr;
};

std::string Capital(const char* w)
{
    std::string s = w ? w : "";
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

// The user's name of a node: its own name; an unnamed Switch: its group's name; an unnamed child of a Switch: the value it
// is the case of; never the generic word unless nothing else is known.
std::string NodeName(const Ctx& c, const std::string& id)
{
    if (id == "grp") return "Switch Groups";
    if (id.compare(0, 4, "grp:") == 0) {
        if (const SwitchGroup* sg = FindGroup(*c.store, id.substr(4))) return sg->name;
        const std::string n = SyncGhostName(*c.d, id);
        return n.empty() ? id.substr(4) : n;
    }
    if (const SoundNode* n = FindNode(*c.lib, id)) {
        if (!n->name.empty()) return n->name;
        if (n->parent.empty()) return c.lib->name;
        if (const SoundNode* p = FindNode(*c.lib, n->parent))
            if (p->type == SoundNodeType::Switch)
                for (const SwitchCase& sc : p->cases)
                    if (sc.child_uid == id)
                        if (const SwitchGroup* sg = FindGroup(*c.store, p->group_uid))
                            for (const SwitchValue& v : sg->values)
                                if (v.uid == sc.value_uid && !v.name.empty()) return v.name;
        if (n->type == SoundNodeType::Switch)
            if (const SwitchGroup* sg = FindGroup(*c.store, n->group_uid))
                if (!sg->name.empty()) return sg->name;
        return Capital(SoundNodeTypeWord(n->type));
    }
    const std::string g2 = SyncGhostName(*c.d, id);
    if (!g2.empty()) return g2;
    const std::string w = SyncGhostType(*c.d, id);
    return w.empty() ? std::string("(new)") : w;
}

bool IsSoundId(const Ctx& c, const std::string& id)
{
    if (const SoundNode* n = FindNode(*c.lib, id)) return n->type == SoundNodeType::Sound;
    const SyncChange* x = c.d->Find("X|" + id);
    return x && !x->adds_container;
}

SoundNodeType TypeOfId(const Ctx& c, const std::string& id)
{
    if (const SoundNode* n = FindNode(*c.lib, id)) return n->type;
    const std::string w = SyncGhostType(*c.d, id);
    if (w == "Blend") return SoundNodeType::Blend;
    if (w == "Switch") return SoundNodeType::Switch;
    if (IsSoundId(c, id)) return SoundNodeType::Sound;
    return SoundNodeType::Random;
}

bool NoneText(const std::string& t)
{
    return t == "absent" || t == "removed";
}

std::string KindOf(const SyncChange& c)
{
    return c.key.substr(0, c.key.find('|'));
}

// What a value cell says for an engine text: "present" is the element itself, bare markers read as "changed".
std::string Show(const std::string& t, const std::string& other, const std::string& label)
{
    if (t == "present") return other == "present" ? std::string("changed") : label;
    if (t == "here" || t == "file" || t.empty()) return "changed";
    if (t == "here order" || t == "global order") return "other order";
    return t;
}

void BuildModel(const Ctx& c, Model* m)
{
    const SyncDiff& d = *c.d;
    // pass 1: the sound names on show (the common start and end of those are hidden)
    std::vector<std::string> sound_names;
    for (const SyncChange& ch : d.changes) {
        const std::string kind = KindOf(ch);
        if (kind == "X" && !ch.adds_container) sound_names.push_back(ch.label);
        else if (ch.label == "Name" && IsSoundId(c, ch.subject)) {
            if (!NoneText(ch.here_text)) sound_names.push_back(ch.here_text);
            if (!NoneText(ch.file_text)) sound_names.push_back(ch.file_text);
        }
        if (kind != "X" && ch.subject != ch.node && IsSoundId(c, ch.subject)) sound_names.push_back(NodeName(c, ch.subject));
    }
    const SyncNameTrim trim = SyncCommonTrim(sound_names);

    std::map<std::string, int> gix;
    for (const SyncChange& ch : d.changes) {
        const std::string kind = KindOf(ch);
        const bool        grp = ch.node.compare(0, 4, "grp:") == 0;
        const std::string gid = grp ? std::string("grp") : ch.node;
        auto              it = gix.find(gid);
        if (it == gix.end()) {
            Group gr;
            gr.id = gid;
            if (gid == "grp") gr.segs.push_back("Switch Groups");
            else {
                for (std::string u = gid; !u.empty() && u != d.root; u = SyncParentOf(d, u)) gr.segs.push_back(NodeName(c, u));
                std::reverse(gr.segs.begin(), gr.segs.end());
                if (gr.segs.empty()) gr.segs.push_back(c.lib->name);
                if (const SoundNode* n = FindNode(*c.lib, gid))
                    if (n->type == SoundNodeType::Switch)
                        if (const SwitchGroup* sg = FindGroup(*c.store, n->group_uid)) gr.hint = "by " + sg->name;
            }
            m->groups.push_back(gr);
            it = gix.emplace(gid, static_cast<int>(m->groups.size()) - 1).first;
        }
        Row r;
        r.key = ch.key;
        r.group = it->second;
        r.side = ch.side;
        const bool sound_subject = !ch.subject.empty() && !grp && IsSoundId(c, ch.subject);
        r.add = SyncIsAddition(ch, &r.adder);
        // the CHANGE column
        if (grp) {
            const std::string gname = NodeName(c, ch.node);
            if (kind == "GX") r.last = gname;
            else {
                r.par = gname;
                r.last = ch.label.empty() ? ch.text : ch.label;
            }
        } else if (kind == "X") {
            r.last = sound_subject ? SyncTrimName(trim, ch.label) : ch.label;
        } else {
            if (!ch.subject.empty() && ch.subject != ch.node) {
                const std::string sn = NodeName(c, ch.subject);
                r.par = sound_subject ? SyncTrimName(trim, sn) : sn;
            }
            r.last = ch.label.empty() ? ch.text : ch.label;
        }
        r.full_name = r.par.empty() ? r.last : r.par + kArrow + r.last;
        // the two cells
        const bool trim_vals = sound_subject && (kind == "X" || ch.label == "Name");
        auto       tr = [&](const std::string& s) { return trim_vals ? SyncTrimName(trim, s) : s; };
        r.gnone = NoneText(ch.file_text);
        r.pnone = NoneText(ch.here_text);
        const std::string elem = kind == "X" ? ch.label : (ch.label.empty() ? ch.text : ch.label);
        if (r.add) {
            // an addition: the adding side shows "+ name · content", the other side nothing
            const std::vector<std::string> dep = DependentsClosure(d, ch.key);
            int                            sounds = 0, values = 0;
            for (const std::string& k : dep) {
                const SyncChange* x = d.Find(k);
                if (!x) continue;
                if (x->key.compare(0, 3, "VX|") == 0) ++values;
                else if (x->key.compare(0, 2, "X|") == 0 && !x->adds_container) ++sounds;
                r.tree.push_back(x->text);
            }
            std::string info;
            if (sounds > 0) info = std::to_string(sounds) + (sounds == 1 ? " Sound" : " Sounds");
            else if (values > 0) info = std::to_string(values) + (values == 1 ? " value" : " values");
            const std::string cell = "+ " + tr(elem) + (info.empty() ? std::string() : kDot + info);
            const std::string full = "+ " + elem + (info.empty() ? std::string() : kDot + info);
            if (r.adder == SyncSide::Here) {
                r.ptxt = cell;
                r.pfull = full;
            } else {
                r.gtxt = cell;
                r.gfull = full;
            }
            r.gnone = r.adder == SyncSide::Here;
            r.pnone = r.adder == SyncSide::File;
        } else {
            r.gfull = r.gnone ? std::string() : Show(ch.file_text, ch.here_text, elem);
            r.pfull = r.pnone ? std::string() : Show(ch.here_text, ch.file_text, elem);
            r.gtxt = tr(r.gfull);
            r.ptxt = tr(r.pfull);
            if (!r.gnone && !r.pnone && r.gtxt != r.ptxt) SyncDiffRange(r.gtxt, r.ptxt, &r.glo, &r.ghi, &r.plo, &r.phi);
        }
        if (kind == "X") r.icon = static_cast<int>(TypeOfId(c, ch.subject));
        else if (kind == "GX") r.icon = 100 + kGlyphGroup;
        else if (kind == "VX") r.icon = 100 + kGlyphValue;
        else if (kind == "C") r.icon = static_cast<int>(SoundNodeType::Switch);
        m->row_of[r.key] = static_cast<int>(m->rows.size());
        m->groups[static_cast<size_t>(r.group)].rows.push_back(static_cast<int>(m->rows.size()));
        m->groups[static_cast<size_t>(r.group)].keys.push_back(r.key);
        m->all.push_back(r.key);
        m->rows.push_back(std::move(r));
    }
    // the groups in path order (the library's top first, Switch Groups last); rows keep the diff's order
    std::vector<int> order(m->groups.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return s;
    };
    auto path_of = [&](const Group& gr) {
        std::string s;
        for (const std::string& x : gr.segs) s += lower(x) + "\x1f";
        return s;
    };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const Group &ga = m->groups[static_cast<size_t>(a)], &gb = m->groups[static_cast<size_t>(b)];
        const int ra = ga.id == "grp" ? 2 : ga.id == d.root ? 0 : 1, rb = gb.id == "grp" ? 2 : gb.id == d.root ? 0 : 1;
        if (ra != rb) return ra < rb;
        return path_of(ga) < path_of(gb);
    });
    std::vector<Group> sorted;
    std::vector<int>   remap(order.size());
    for (size_t i = 0; i < order.size(); ++i) {
        remap[static_cast<size_t>(order[i])] = static_cast<int>(i);
        sorted.push_back(m->groups[static_cast<size_t>(order[i])]);
    }
    m->groups.swap(sorted);
    for (Row& r : m->rows) r.group = remap[static_cast<size_t>(r.group)];
    for (size_t i = 0; i < m->groups.size(); ++i) m->group_of[m->groups[i].id] = static_cast<int>(i);
}

bool IsOpen(const std::string& id)
{
    const auto it = g.fold.find(id);
    return it == g.fold.end() || it->second;
}

// ---- small drawing helpers ----------------------------------------------------------------------------------------

float RowH()
{
    return ui::Dp(24.0f);
}

bool HoverRect(ImVec2 a, ImVec2 b)
{
    return ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a, b, false);
}

void TipRect(ImVec2 a, ImVec2 b, const std::string& tip)
{
    if (!tip.empty() && HoverRect(a, b)) ui::ShowTip(tip.c_str());
}

ImU32 WithAlpha(ImU32 c, int a)
{
    return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

const ImU32 kWarmBox = IM_COL32(0xD9, 0xB4, 0x4A, 0xFF);
const ImU32 kHlNeutral = IM_COL32(255, 255, 255, 0x1A);
const ImU32 kHlGlobal = IM_COL32(255, 209, 102, 0x3D);
const ImU32 kHlProject = IM_COL32(91, 140, 255, 0x4D);
const ImU32 kWarmInk = IM_COL32(0xCD, 0xB4, 0x6A, 0xFF);

// The columns of every row: [name] [box] [GLOBAL] [connector] [box] [THIS PROJECT]; fixed, so marks never move.
struct Cols {
    float x0 = 0, w = 0;
    float name_x = 0, name_w = 0;
    float gbox_x = 0, g_x = 0, g_w = 0;
    float conn_x = 0, conn_w = 0;
    float pbox_x = 0, p_x = 0, p_w = 0;
};

Cols MakeCols(float x0, float w)
{
    Cols c;
    c.x0 = x0;
    c.w = w;
    const float gap = ui::Dp(4.0f), box = ui::Dp(20.0f);
    c.conn_w = ui::Dp(30.0f);
    const float avail = std::max(ui::Dp(120.0f), w - 2.0f * box - c.conn_w - 5.0f * gap - ui::Dp(8.0f));
    float       name = std::max(ui::Dp(150.0f), avail * 0.8f / 2.8f);
    name = std::min(name, std::max(ui::Dp(70.0f), avail - 2.0f * ui::Dp(60.0f)));
    const float val = std::max(ui::Dp(30.0f), (avail - name) * 0.5f);
    float       x = x0 + ui::Dp(4.0f);
    c.name_x = x;
    c.name_w = name;
    x += name + gap;
    c.gbox_x = x;
    x += box + gap;
    c.g_x = x;
    c.g_w = val;
    x += val + gap;
    c.conn_x = x;
    x += c.conn_w + gap;
    c.pbox_x = x;
    x += box + gap;
    c.p_x = x;
    c.p_w = val;
    return c;
}

// A side's box at (x, row top), centred on the row; clicked = true. state 0 off, 1 on, 2 mixed. `warm` = the global's tint,
// `quiet` = a group's "keep all" (paler).
bool SideBox(const char* id, float x, float y, float rowh, int state, bool warm, bool quiet, const char* tip)
{
    const float box = ui::Dp(14.0f);
    ImGui::SetCursorScreenPos(ImVec2(std::floor(x + (ui::Dp(20.0f) - box) * 0.5f), std::floor(y + (rowh - box) * 0.5f)));
    const bool   clicked = ImGui::InvisibleButton(id, ImVec2(box, box));
    const bool   hovered = ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin(), b(a.x + box, a.y + box);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  r = ui::Dp(3.0f), k = box / 14.0f;
    if (state != 0) {
        ImU32 fill = warm ? kWarmBox : ui::kAccent;
        if (quiet) fill = WithAlpha(fill, hovered ? 0xC0 : 0x8C);
        else if (hovered) fill = warm ? IM_COL32(0xE6, 0xC4, 0x5A, 0xFF) : ui::kPrimaryHover;
        const ImU32 ink = warm ? IM_COL32(0x1B, 0x1D, 0x22, 0xFF) : IM_COL32_WHITE;
        dl->AddRectFilled(a, b, fill, r);
        if (state == 2) {
            const float yy = std::floor(a.y + box * 0.5f) + 0.5f;
            dl->AddLine(ImVec2(a.x + 3.5f * k, yy), ImVec2(b.x - 3.5f * k, yy), ink, std::max(1.5f, 1.8f * k));
        } else {
            const ImVec2 tick[3] = {ImVec2(a.x + 3.2f * k, a.y + 7.2f * k), ImVec2(a.x + 5.9f * k, a.y + 9.9f * k),
                                    ImVec2(a.x + 10.8f * k, a.y + 4.2f * k)};
            dl->AddPolyline(tick, 3, ink, ImDrawFlags_None, std::max(1.5f, 1.8f * k));
        }
    } else {
        dl->AddRectFilled(a, b, ui::kBg, r);
        dl->AddRect(a, b, hovered ? (warm ? kWarmBox : ui::kAccent) : (quiet ? ui::kStroke : ui::kStrokeStrong), r);
    }
    if (hovered && tip && *tip) ui::ItemTipText(tip);
    return clicked;
}

// The state glyph between the two sides: points to the kept side. mode 0 skipped, 1 global kept, 2 project kept, 4 pick (3 was "both kept": v5 has no such row).
void DrawConn(ImDrawList* dl, const Cols& c, float y, float rowh, int mode, bool dim)
{
    const float cx = std::floor(c.conn_x + c.conn_w * 0.5f), cy = std::floor(y + rowh * 0.5f), s = ui::Dp(4.5f);
    auto tri = [&](float x, bool left, ImU32 col) {
        if (left) dl->AddTriangleFilled(ImVec2(x + s, cy - s), ImVec2(x + s, cy + s), ImVec2(x - s, cy), col);
        else dl->AddTriangleFilled(ImVec2(x - s, cy - s), ImVec2(x - s, cy + s), ImVec2(x + s, cy), col);
    };
    const char* tip = "Skipped: stays different, still listed after Apply";
    if (mode == 1) {
        tri(cx, true, kWarmBox);
        tip = "The global's is kept: this project takes it";
    } else if (mode == 2) {
        tri(cx, false, ui::kAccent);
        tip = "This project's is kept: the global takes it";
    } else if (mode == 4) {
        ImFont*      f = ui::FontSmall();
        const char*  t = "PICK";
        const ImVec2 ts = ui::TextSizeIn(f, t);
        const ImVec2 a(cx - ts.x * 0.5f - ui::Dp(3.0f), cy - ts.y * 0.5f - ui::Dp(1.0f)), b(cx + ts.x * 0.5f + ui::Dp(3.0f), cy + ts.y * 0.5f + ui::Dp(1.0f));
        dl->AddRect(a, b, ui::kBothLine, ui::Dp(3.0f));
        ui::AddTextIn(dl, f, ImVec2(a.x + ui::Dp(3.0f), a.y + ui::Dp(1.0f)), ui::kBoth, t);
        tip = "Changed on both sides: tick the one you keep";
    } else {
        dl->AddCircleFilled(ImVec2(cx, cy), ui::Dp(1.6f), ui::kFaint);
    }
    (void)dim;
    TipRect(ImVec2(c.conn_x, y), ImVec2(c.conn_x + c.conn_w, y + rowh), tip);
}

// A value in [x, x + maxw): the differing part [lo, hi) highlighted; too long, the head shrinks with "..." and the last 9
// characters stay (the differing part never hides); `strike`: a line through it (the side that gets replaced).
void DrawValue(ImDrawList* dl, ImFont* f, float x, float y, float maxw, ImU32 ink, const std::string& str, size_t lo, size_t hi,
               ImU32 hl, bool strike)
{
    if (str.empty() || maxw < 4.0f) return;
    auto W = [&](const std::string& s) { return ui::TextSizeIn(f, s.c_str()).x; };
    const float h = f->FontSize;
    float       cx = x;
    auto seg = [&](const std::string& s, size_t from, size_t to, size_t off) {  // chars [from, to) of s, highlighted where in [lo, hi) (+off)
        const size_t hs = hi > lo ? lo + 0 : 0, he = hi > lo ? hi : 0;
        size_t       a = from;
        while (a < to) {
            const bool   in = he > hs && a + off >= hs && a + off < he;
            size_t       b = a;
            while (b < to && (he > hs && b + off >= hs && b + off < he) == in) ++b;
            const std::string piece = s.substr(a, b - a);
            const float       pw = W(piece);
            if (in && hl) dl->AddRectFilled(ImVec2(cx - 1.0f, y), ImVec2(cx + pw + 1.0f, y + h + 1.0f), hl, 3.0f);
            ui::AddTextIn(dl, f, ImVec2(cx, y), ink, piece.c_str());
            cx += pw;
            a = b;
        }
    };
    if (W(str) <= maxw) {
        seg(str, 0, str.size(), 0);
    } else {
        bool done = false;
        if (str.size() > 18) {
            size_t k = str.size() - 9;
            if (hi > lo) k = std::min(k, lo);
            const std::string tail = str.substr(k);
            const float       tw = W(tail);
            if (tw + W("...") < maxw) {
                const std::string head = ui::EllipsizeToWidth(f, str.substr(0, k), maxw - tw);
                seg(head, 0, head.size(), static_cast<size_t>(-1) / 2);  // the head is never highlighted
                seg(tail, 0, tail.size(), k);
                done = true;
            }
        }
        if (!done) {
            const std::string e = ui::EllipsizeToWidth(f, str, maxw);
            seg(e, 0, e.size(), static_cast<size_t>(-1) / 2);
        }
    }
    if (strike) dl->AddLine(ImVec2(x, y + h * 0.55f), ImVec2(cx, y + h * 0.55f), WithAlpha(ui::kBad, 0xC0));
}

void DrawIcon(ImDrawList* dl, ImVec2 p, float s, int icon, ImU32 col)
{
    if (icon < 0) return;
    if (icon >= 100) DrawLibraryGlyph(dl, p, s, icon - 100, col);
    else DrawNodeTypeGlyph(dl, p, s, static_cast<SoundNodeType>(icon), col);
}

// ---- gestures ---------------------------------------------------------------------------------------------------

// Folds open on what a gesture ticked for the user (so the rows that say why are visible).
void OpenTicked(const SyncDiff& d, const SyncEffect& fx)
{
    for (const std::string& t : fx.ticked)
        if (const SyncChange* x = d.Find(t)) g.fold[x->node.compare(0, 4, "grp:") == 0 ? std::string("grp") : x->node] = true;
}

void BoxGesture(const std::string& key, SyncDir dir)
{
    Defer([key, dir] {
        const SyncDiff* d = LibrarySyncDiff(g.lib);
        if (!d) return;
        const SyncEffect fx = g.sel.ToggleSide(*d, key, dir);
        OpenTicked(*d, fx);
        SetToast(SyncEffectToast(*d, fx));
    });
}

void KeepGesture(const std::vector<std::string>& keys, SyncDir dir)
{
    Defer([keys, dir] {
        const SyncDiff* d = LibrarySyncDiff(g.lib);
        if (!d) return;
        const SyncEffect fx = g.sel.KeepTri(*d, keys, dir) == SyncTri::All ? g.sel.KeepClear(*d, keys, dir) : g.sel.KeepAll(*d, keys, dir);
        OpenTicked(*d, fx);
        SetToast(SyncEffectToast(*d, fx));
    });
}

int TriInt(SyncTri t)
{
    return t == SyncTri::All ? 1 : t == SyncTri::Some ? 2 : 0;
}

// ---- the rows -----------------------------------------------------------------------------------------------------

float StripHeight(const Row& r)
{
    const float lh = ui::FontSmall()->FontSize + ui::Dp(3.0f);
    size_t      n = 1;
    if (r.add) n = 1 + std::min<size_t>(r.tree.size() + (r.tree.size() > 8 ? 1 : 0), 9);
    return static_cast<float>(n) * lh + ui::Dp(10.0f);
}

void DrawStrip(const Model& m, const Row& r, const Cols& c)
{
    (void)m;
    const float      h = StripHeight(r);
    const ImVec2     p = ImGui::GetCursorScreenPos();
    ImDrawList*      dl = ImGui::GetWindowDrawList();
    ImFont*          f = ui::FontSmall();
    const float      lh = f->FontSize + ui::Dp(3.0f);
    dl->AddRectFilled(p, ImVec2(p.x + c.w, p.y + h), ui::kRaised, ui::kRadiusSm, ImDrawFlags_RoundCornersBottom);
    const char* where = r.side == SyncSide::Both ? "Changed on both sides"
                        : r.add ? (r.adder == SyncSide::Here ? "Added in this project" : "Added in the global")
                                : (r.side == SyncSide::Here ? "Changed in this project" : "Changed in the global");
    ui::EllipsisTextTip(dl, ImVec2(c.name_x + ui::Dp(22.0f), p.y + ui::Dp(4.0f)), c.name_x + c.name_w, ui::kMuted, where);
    auto column = [&](float x, float w, bool none, const std::string& full, bool adder_side, const char* nothing) {
        float y = p.y + ui::Dp(4.0f);
        const float mx = x + w - ui::Dp(6.0f);
        if (none) {
            ui::EllipsisTextTip(dl, ImVec2(x + ui::Dp(6.0f), y), mx, ui::kFaint, nothing);
            return;
        }
        ui::EllipsisTextTip(dl, ImVec2(x + ui::Dp(6.0f), y), mx, ui::kText, full.empty() ? std::string("changed") : full);
        if (!adder_side) return;
        const size_t shown = std::min<size_t>(r.tree.size(), 8);
        for (size_t i = 0; i < shown; ++i) {
            y += lh;
            dl->AddLine(ImVec2(x + ui::Dp(8.0f), y - 1.0f), ImVec2(x + ui::Dp(8.0f), y + lh - 1.0f), ui::kStrokeStrong);
            ui::EllipsisTextTip(dl, ImVec2(x + ui::Dp(14.0f), y), mx, ui::kMuted, r.tree[i]);
        }
        if (r.tree.size() > shown) {
            y += lh;
            ui::EllipsisTextTip(dl, ImVec2(x + ui::Dp(14.0f), y), mx, ui::kFaint, "+" + std::to_string(r.tree.size() - shown) + " more");
        }
    };
    column(c.g_x, c.g_w, r.gnone, r.gfull, r.add && r.adder == SyncSide::File, "Not in the global");
    column(c.p_x, c.p_w, r.pnone, r.pfull, r.add && r.adder == SyncSide::Here, "Not in this project");
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(c.w, h));
}

// One side's value cell. kept: this side's version wins; decided: some side is kept; changed: this side moved.
void DrawCell(ImDrawList* dl, const Row& r, bool glob, const Cols& c, float y, float rowh, bool kept, bool decided, bool changed)
{
    const bool        none = glob ? r.gnone : r.pnone;
    const std::string& txt = glob ? r.gtxt : r.ptxt;
    const float       x = glob ? c.g_x : c.p_x, w = glob ? c.g_w : c.p_w;
    const float       ch = ui::Dp(20.0f), cy = std::floor(y + (rowh - ch) * 0.5f);
    if (kept) dl->AddRectFilled(ImVec2(x, cy), ImVec2(x + w, cy + ch), glob ? ui::kWarnSoft : ui::kAccentSoft, ui::Dp(4.0f));
    ImFont*     f = ui::FontBody();
    const float ty = std::floor(cy + (ch - f->FontSize) * 0.5f);
    float       tx = x + ui::Dp(6.0f);
    const float right = x + w - ui::Dp(6.0f);
    if (none) {
        ui::AddTextIn(dl, f, ImVec2(tx, ty), ui::kFaint, kDash);
        return;
    }
    ImU32 ink = ui::kText, hl = kHlNeutral;
    bool  strike = false;
    if (kept) hl = glob ? kHlGlobal : kHlProject;
    else if (decided) {
        ink = ui::kFaint;
        hl = 0;
        strike = true;
    } else if (!changed) {
        ink = ui::kMuted;
        hl = 0;
    }
    if (r.icon >= 0) {
        const float is = ui::Dp(13.0f);
        DrawIcon(dl, ImVec2(tx, std::floor(cy + (ch - is) * 0.5f)), is, r.icon, kept ? (glob ? kWarmInk : ui::kAccentInk) : ui::kMuted);
        tx += is + ui::Dp(5.0f);
    }
    DrawValue(dl, f, tx, ty, std::max(1.0f, right - tx), ink, txt, glob ? r.glo : r.plo, glob ? r.ghi : r.phi, hl, strike);
}

void DrawChangeRow(const Ctx& cx, const Model& m, int ri, const Cols& c)
{
    const Row&        r = m.rows[static_cast<size_t>(ri)];
    const SyncDiff&   d = *cx.d;
    const SyncChange* ch = d.Find(r.key);
    if (!ch) return;
    const float  rowh = RowH();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool   open = g.open_row == r.key;
    const float  total = rowh + (open ? StripHeight(r) : 0.0f);
    if (!ImGui::IsRectVisible(p, ImVec2(p.x + c.w, p.y + total))) {  // H1: a row out of view only takes its room
        ImGui::Dummy(ImVec2(c.w, total));
        return;
    }
    ImDrawList*     dl = ImGui::GetWindowDrawList();
    const SyncTick* tk = g.sel.Tick(r.key);
    const bool      on = g.sel.IsOn(r.key);
    const bool      kept_g = on && tk && tk->dir == SyncDir::Down, kept_p = on && tk && tk->dir == SyncDir::Up;
    const bool      decided = on;
    if (open) dl->AddRectFilled(p, ImVec2(p.x + c.w, p.y + rowh), ui::kRaised, ui::kRadiusSm, ImDrawFlags_RoundCornersTop);
    else if (HoverRect(p, ImVec2(p.x + c.w, p.y + rowh))) dl->AddRectFilled(p, ImVec2(p.x + c.w, p.y + rowh), ui::kSurface, ui::kRadiusSm);
    ImGui::PushID(r.key.c_str());
    // the boxes: one per side; an addition has only the adding side's
    const bool add_here = r.add && r.adder == SyncSide::Here, add_glob = r.add && r.adder == SyncSide::File;
    const char* tipg = add_glob ? "Add it to this project too (unticked: it stays only in the global)"
                                : r.add ? "" : "Keep the global's";
    const char* tipp = add_here ? "Add it to the global too (unticked: it stays only in this project)"
                                : r.add ? "" : "Keep this project's";
    if (!add_here && SideBox("##g", c.gbox_x, p.y, rowh, kept_g ? 1 : 0, true, false, tipg)) BoxGesture(r.key, SyncDir::Down);
    if (!add_glob && SideBox("##p", c.pbox_x, p.y, rowh, kept_p ? 1 : 0, false, false, tipp)) BoxGesture(r.key, SyncDir::Up);
    // the cells (a click opens the detail strip) and the state glyph
    const float ch2 = ui::Dp(20.0f), cy = std::floor(p.y + (rowh - ch2) * 0.5f);
    const bool  gchanged = ch->side == SyncSide::Both || ch->side == SyncSide::File;
    const bool  pchanged = ch->side == SyncSide::Both || ch->side == SyncSide::Here;
    DrawCell(dl, r, true, c, p.y, rowh, kept_g, decided, gchanged);
    DrawCell(dl, r, false, c, p.y, rowh, kept_p, decided, pchanged);
    const int mode = kept_g ? 1 : kept_p ? 2 : (ch->side == SyncSide::Both ? 4 : 0);
    DrawConn(dl, c, p.y, rowh, mode, false);
    bool toggled = false;
    ImGui::SetCursorScreenPos(ImVec2(c.g_x, cy));
    toggled |= ImGui::InvisibleButton("##gc", ImVec2(c.g_w, ch2));
    if (ImGui::IsItemHovered() && !r.gfull.empty()) ui::ItemTipText(("Global: " + r.gfull + "\nClick for details").c_str());
    else if (ImGui::IsItemHovered()) ui::ItemTipText(r.gnone ? "Not in the global" : "Global");
    ImGui::SetCursorScreenPos(ImVec2(c.p_x, cy));
    toggled |= ImGui::InvisibleButton("##pc", ImVec2(c.p_w, ch2));
    if (ImGui::IsItemHovered() && !r.pfull.empty()) ui::ItemTipText(("This project: " + r.pfull + "\nClick for details").c_str());
    else if (ImGui::IsItemHovered()) ui::ItemTipText(r.pnone ? "Not in this project" : "This project");
    // the name column: path (muted) + name, then "needed by ..." when it was ticked for you
    ImFont*     nf = ui::FontBody();
    const float ty = std::floor(p.y + (rowh - nf->FontSize) * 0.5f);
    const float nx = c.name_x + ui::Dp(22.0f), nmax = c.name_x + c.name_w;
    {
        float x = nx;
        std::string chip, chip_tip;
        const SyncTick* bt = on && tk && tk->auto_ ? g.sel.Tick(tk->by) : nullptr;
        if (bt && bt->on && bt->dir == tk->dir) {
            const auto it = m.row_of.find(tk->by);
            chip = std::string(tk->needs_why ? "needed by " : "goes with ") + (it == m.row_of.end() ? tk->by : m.rows[static_cast<size_t>(it->second)].full_name);
            chip_tip = tk->needs_why ? "Ticked for you: the change it belongs to needs this one" : "Ticked for you: it goes away with the change it belongs to";
        } else if (on) {
            const auto nbit = g.nb_cache.find(r.key);  // H1: computed once per selection change, not per row per frame
            if (nbit != g.nb_cache.end() && !nbit->second.empty()) {
                const auto first = m.row_of.find(nbit->second[0]);
                chip = "needed by " + (first == m.row_of.end() ? nbit->second[0] : m.rows[static_cast<size_t>(first->second)].full_name) +
                       (nbit->second.size() > 1 ? " +" + std::to_string(nbit->second.size() - 1) : std::string());
                chip_tip = "Another ticked change needs this one";
            }
        }
        const float chip_room = chip.empty() ? 0.0f : std::min(ui::Dp(170.0f), (nmax - nx) * 0.45f);
        const float room = nmax - x - (chip_room > ui::Dp(50.0f) ? chip_room + ui::Dp(6.0f) : 0.0f);
        const std::string sep = r.par.empty() ? std::string() : std::string(kArrow);
        const float lastw = ui::TextSizeIn(nf, r.last.c_str()).x;
        const float parw = r.par.empty() ? 0.0f : ui::TextSizeIn(nf, (r.par + sep).c_str()).x;
        std::string tip = r.full_name;
        if (!ch->needs.empty()) {
            tip += "\nNeeds: ";
            for (size_t i = 0; i < ch->needs.size(); ++i) {
                const auto nr = m.row_of.find(ch->needs[i]);
                tip += std::string(i ? ", " : "") + (nr == m.row_of.end() ? ch->needs[i] : m.rows[static_cast<size_t>(nr->second)].full_name);
            }
        }
        float used = 0.0f;
        if (!r.par.empty() && parw + lastw <= room) {
            ui::AddTextIn(dl, nf, ImVec2(x, ty), ui::kMuted, (r.par + sep).c_str());
            ui::AddTextIn(dl, nf, ImVec2(x + parw, ty), ui::kText, r.last.c_str());
            used = parw + lastw;
        } else if (!r.par.empty() && room - lastw > ui::Dp(30.0f)) {
            const std::string pe = ui::EllipsizeToWidth(nf, r.par, room - lastw - ui::TextSizeIn(nf, sep.c_str()).x);
            const float       pw = ui::TextSizeIn(nf, (pe + sep).c_str()).x;
            ui::AddTextIn(dl, nf, ImVec2(x, ty), ui::kMuted, (pe + sep).c_str());
            ui::AddTextIn(dl, nf, ImVec2(x + pw, ty), ui::kText, r.last.c_str());
            used = pw + lastw;
        } else {
            const std::string le = ui::EllipsizeToWidth(nf, r.last, room);
            ui::AddTextIn(dl, nf, ImVec2(x, ty), ui::kText, le.c_str());
            used = ui::TextSizeIn(nf, le.c_str()).x;
        }
        TipRect(ImVec2(x, p.y), ImVec2(x + std::max(used, 1.0f), p.y + rowh), tip);
        if (chip_room > ui::Dp(50.0f)) {
            ImFont*           sf = ui::FontSmall();
            const std::string shown = ui::EllipsizeToWidth(sf, chip, chip_room);
            const float       cxp = x + used + ui::Dp(6.0f);
            ui::AddTextIn(dl, sf, ImVec2(cxp, std::floor(p.y + (rowh - sf->FontSize) * 0.5f)), WithAlpha(ui::kAccentInk, 0xCC), shown.c_str());
            TipRect(ImVec2(cxp, p.y), ImVec2(cxp + ui::TextSizeIn(sf, shown.c_str()).x, p.y + rowh), shown != chip ? chip_tip + "\n" + chip : chip_tip);
        }
        ImGui::SetCursorScreenPos(ImVec2(c.name_x, p.y));
        toggled |= ImGui::InvisibleButton("##nm", ImVec2(c.name_w, rowh));
    }
    if (toggled) g.open_row = open ? std::string() : r.key;
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowh));
    if (open) DrawStrip(m, r, c);
    else ImGui::Dummy(ImVec2(c.w, 0.0f));
}

void DrawGroupRow(const Ctx& cx, const Model& m, int gi, const Cols& c)
{
    const Group&    gr = m.groups[static_cast<size_t>(gi)];
    const SyncDiff& d = *cx.d;
    const float     rowh = RowH();
    const ImVec2    p = ImGui::GetCursorScreenPos();
    ImDrawList*     dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ui::Dp(2.0f)));
    const ImVec2 q = ImGui::GetCursorScreenPos();
    if (g.scroll_to == gr.id) {
        ImGui::Dummy(ImVec2(1.0f, rowh));
        ImGui::SetScrollHereY(0.25f);
        g.scroll_to.clear();
        ImGui::SetCursorScreenPos(q);
    }
    if (HoverRect(q, ImVec2(q.x + c.w, q.y + rowh))) dl->AddRectFilled(q, ImVec2(q.x + c.w, q.y + rowh), ui::kHover, ui::kRadiusSm);
    ImGui::PushID(gr.id.c_str());
    const bool  open = IsOpen(gr.id);
    const float cs = ui::Dp(18.0f);
    ImGui::SetCursorScreenPos(ImVec2(c.name_x, std::floor(q.y + (rowh - cs) * 0.5f)));
    if (ui::Chevron("##chev", open, cs)) g.fold[gr.id] = !open;
    // the path: muted parents, then the container's own name, then the count
    ImFont*     nf = ui::FontSemibold();
    const float ty = std::floor(q.y + (rowh - nf->FontSize) * 0.5f);
    float       x = c.name_x + cs + ui::Dp(4.0f);
    const float nmax = c.name_x + c.name_w;
    std::string cnt = std::to_string(gr.rows.size());
    const float cnt_w = ui::TextSizeIn(ui::FontSmall(), cnt.c_str()).x + ui::Dp(6.0f);
    std::string path;
    for (size_t i = 0; i < gr.segs.size(); ++i) path += (i ? std::string(kArrow) : std::string()) + gr.segs[i];
    const std::string& last = gr.segs.back();
    const float lastw = ui::TextSizeIn(nf, last.c_str()).x;
    const float room = nmax - x - cnt_w;
    float       used = 0.0f;
    if (gr.segs.size() > 1) {
        std::string par;
        for (size_t i = 0; i + 1 < gr.segs.size(); ++i) par += gr.segs[i] + kArrow;
        const float parw = ui::TextSizeIn(nf, par.c_str()).x;
        if (parw + lastw <= room) {
            ui::AddTextIn(dl, nf, ImVec2(x, ty), ui::kMuted, par.c_str());
            ui::AddTextIn(dl, nf, ImVec2(x + parw, ty), ui::kText, last.c_str());
            used = parw + lastw;
        } else if (room - lastw > ui::Dp(30.0f)) {
            const std::string pe = ui::EllipsizeToWidth(nf, par, room - lastw);
            const float       pw = ui::TextSizeIn(nf, pe.c_str()).x;
            ui::AddTextIn(dl, nf, ImVec2(x, ty), ui::kMuted, pe.c_str());
            ui::AddTextIn(dl, nf, ImVec2(x + pw, ty), ui::kText, last.c_str());
            used = pw + lastw;
        } else {
            const std::string le = ui::EllipsizeToWidth(nf, last, room);
            ui::AddTextIn(dl, nf, ImVec2(x, ty), ui::kText, le.c_str());
            used = ui::TextSizeIn(nf, le.c_str()).x;
        }
    } else {
        const std::string le = ui::EllipsizeToWidth(nf, last, room);
        ui::AddTextIn(dl, nf, ImVec2(x, ty), ui::kText, le.c_str());
        used = ui::TextSizeIn(nf, le.c_str()).x;
    }
    ui::AddTextIn(dl, ui::FontSmall(), ImVec2(x + used + ui::Dp(6.0f), std::floor(q.y + (rowh - ui::FontSmall()->FontSize) * 0.5f)), ui::kFaint, cnt.c_str());
    // the name opens the container in the Library tree (a real node of this project; Switch Groups: nothing)
    const bool clickable = gr.id != "grp" && d.mine_nodes.count(gr.id) != 0;
    ImGui::SetCursorScreenPos(ImVec2(x, ty));
    bool clicked = false;
    if (clickable && used > 0.0f) {
        clicked = ImGui::InvisibleButton("##nm", ImVec2(used, nf->FontSize + 2.0f));
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ui::ItemTipText((path + "\nShow in the tree").c_str());
        }
    } else {
        TipRect(ImVec2(x, ty), ImVec2(x + std::max(1.0f, used), ty + nf->FontSize), path);
    }
    if (clicked) {
        const std::string id = gr.id;
        Pick([id] {
            const SoundLibrary* l = FindLibrary(GetSoundStore(nullptr), g.lib);
            if (l && FindNode(*l, id)) ShowInLibrary(g.lib, RowOfNode(*l, id));
        });
    }
    // Keep all global / Keep all mine for this group (paler boxes), the hint or a folded summary between them
    const SyncTri tl = g.sel.KeepTri(d, gr.keys, SyncDir::Down), tp = g.sel.KeepTri(d, gr.keys, SyncDir::Up);
    if (SideBox("##kg", c.gbox_x, q.y, rowh, TriInt(tl), true, true, "Keep all global in this container")) KeepGesture(gr.keys, SyncDir::Down);
    if (SideBox("##kp", c.pbox_x, q.y, rowh, TriInt(tp), false, true, "Keep all mine in this container")) KeepGesture(gr.keys, SyncDir::Up);
    std::string sum = gr.hint;
    if (!open) {
        int gl = 0, pr = 0, both = 0, none = 0;
        for (int ri : gr.rows) {
            const SyncTick* t = g.sel.Tick(m.rows[static_cast<size_t>(ri)].key);
            if (!t || !t->on) ++none;
            else (t->dir == SyncDir::Down ? gl : pr)++;
        }
        (void)both;
        sum += std::string(sum.empty() ? "" : kDot) + std::to_string(gl) + " global" + kDot + std::to_string(pr) + " mine";
        if (none > 0) sum += kDot + std::to_string(none) + " left";
    }
    if (!sum.empty())
        ui::EllipsisTextTip(dl, ImVec2(c.g_x + ui::Dp(6.0f), std::floor(q.y + (rowh - ui::FontSmall()->FontSize) * 0.5f)), c.conn_x + c.conn_w,
                            ui::kFaint, sum);
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(p.x, q.y + rowh));
    ImGui::Dummy(ImVec2(c.w, 0.0f));
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
    const char* title = "Sync with global";
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
    if (!info.by.empty()) line1 = "Global library written by " + info.by + (info.at > 0 ? ", " + When(info.at) : std::string());
    const std::string synced = info.synced_at > 0 ? "this project last synced " + When(info.synced_at) : std::string("this project never synced");
    line1 += (line1.empty() ? std::string() : std::string(kDot)) + synced + ".";
    ui::ToneText(line1, ui::kMuted, true);
    ui::ToneText("Nothing is written until you click Apply. Tick the version you keep: the other side takes it. An added element has "
                 "one box (tick it to add it to the other side too).",
                 ui::kMuted, true);
}

// The fixed header: the three column titles and the two "Keep all" boxes of the whole list.
void DrawHeader(const SoundLibrary* lib, const Model& m, const SyncDiff& d, const Cols& c)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  l1 = ui::Dp(20.0f), l2 = ui::Dp(22.0f);
    ImFont*      f = ui::FontSmall();
    auto cap = [&](float x, float maxx, ImU32 col, const char* a, const std::string& b) {
        const float y = std::floor(p.y + (l1 - f->FontSize) * 0.5f);
        ui::AddTextIn(dl, f, ImVec2(x, y), col, a);
        const float aw = ui::TextSizeIn(f, a).x + ui::Dp(6.0f);
        if (!b.empty() && maxx - x - aw > ui::Dp(20.0f)) ui::EllipsisTextTip(dl, ImVec2(x + aw, y), maxx, ui::kFaint, b);
    };
    cap(c.name_x, c.name_x + c.name_w, ui::kFaint, "CHANGE", "\xC2\xB7 " + std::to_string(m.rows.size()));
    cap(c.g_x + ui::Dp(6.0f), c.g_x + c.g_w, kWarmInk, "GLOBAL", lib ? "\xC2\xB7 " + lib->ravlib_path : std::string());
    cap(c.p_x + ui::Dp(6.0f), c.p_x + c.p_w, IM_COL32(0x8E, 0xAA, 0xF0, 0xFF), "THIS PROJECT", lib ? "\xC2\xB7 " + lib->name : std::string());
    const float y2 = p.y + l1;
    ImGui::PushID("##hdr");
    const SyncTri tl = g.sel.KeepTri(d, m.all, SyncDir::Down), tp = g.sel.KeepTri(d, m.all, SyncDir::Up);
    if (SideBox("##kg", c.gbox_x, y2, l2, TriInt(tl), true, false, "Keep all global")) KeepGesture(m.all, SyncDir::Down);
    if (SideBox("##kp", c.pbox_x, y2, l2, TriInt(tp), false, false, "Keep all mine")) KeepGesture(m.all, SyncDir::Up);
    ImGui::PopID();
    const float ty = std::floor(y2 + (l2 - ui::FontBody()->FontSize) * 0.5f);
    auto keep_label = [&](float x, float w, const char* t) {  // ellipsized to its column (a larger font, a narrow window)
        const std::string shown = ui::EllipsizeToWidth(ui::FontBody(), t, std::max(1.0f, w - ui::Dp(6.0f)));
        ui::AddTextIn(dl, ui::FontBody(), ImVec2(x + ui::Dp(6.0f), ty), ui::kMuted, shown.c_str());
    };
    keep_label(c.g_x, c.g_w, "Keep all global");
    keep_label(c.p_x, c.p_w, "Keep all mine");
    dl->AddLine(ImVec2(p.x, p.y + l1 + l2), ImVec2(p.x + c.w, p.y + l1 + l2), ui::kStroke);
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(c.w, l1 + l2 + ui::Dp(2.0f)));
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
    // the summary: two wrapped lines in the room left of the buttons (the tooltip has all of it), "N to pick" on its own
    // line under them, so it never leaves the window
    const float room = std::max(ui::Dp(60.0f), w - aw - cw - ui::Dp(18.0f));
    const float lh = ui::FontSmall()->FontSize, y0 = p.y + ui::Dp(4.0f);
    std::string text;
    if (d) {
        if (left <= 0) text = "Nothing left: this project and the global library agree.";
        else if (cnt.ticked <= 0) text = "Nothing ticked. " + std::to_string(left) + (left == 1 ? " difference." : " differences.");
        else text = g.foot_text;
    }
    {
        ImFont*      sf = ui::FontSmall();
        const ImVec2 ts = sf->CalcTextSizeA(sf->FontSize, FLT_MAX, room, text.c_str());
        if (ts.y > lh * 2.5f) text = ui::EllipsizeToWidth(sf, text, room * 2.0f - ui::Dp(24.0f));  // more than two lines: cut
    }
    ImGui::PushClipRect(ImVec2(p.x, y0), ImVec2(p.x + room, y0 + lh * 2.0f + 1.0f), true);
    ImGui::SetCursorScreenPos(ImVec2(p.x, y0));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + room);
    {
        ui::FontScope f(ui::FontSmall());
        ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(d && left == 0 ? ui::kOk : cnt.ticked > 0 ? ui::kText : ui::kMuted));
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    ImGui::PopClipRect();
    if (d && g.foot_pick > 0 && left > 0)
        ui::AddTextIn(dl, ui::FontSmall(), ImVec2(p.x, y0 + lh * 2.0f + ui::Dp(2.0f)), ui::kBoth, (std::to_string(g.foot_pick) + " to pick").c_str());
    if (cnt.ticked > 0 && !g.foot_full.empty()) TipRect(p, ImVec2(p.x + room, p.y + h), g.foot_full);
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
    if (d) {  // the list was rebuilt: ticks stay by key; what is new is pre-ticked on the side that changed
        const uint64_t sig = SigOf(*d);
        if (!g.have_sig || sig != g.sig) {
            g.have_sig = true;
            g.sig = sig;
            g.sel.Rekey(*d);
            g.sel.PreTick(*d);
            g.model = Model();
            Ctx bc;
            bc.d = d;
            bc.store = &store;
            bc.lib = lib;
            BuildModel(bc, &g.model);
            if (!g.open_row.empty() && !g.model.row_of.count(g.open_row)) g.open_row.clear();
            if (g.post_apply) {
                g.post_apply = false;
                if (d->changes.empty()) {
                    g.open = false;  // nothing left: the window closes
                    return;
                }
            }
        } else if (g.post_apply && ClockSeconds() > g.post_deadline) {
            g.post_apply = false;
        }
    }
    if (g.applying && ClockSeconds() > g.applying_until) g.applying = false;  // L1: no answer came: the button comes back
    if (d && (g.nb_ver != g.sel.Version() || g.nb_sig != g.sig)) {
        g.nb_ver = g.sel.Version();
        g.nb_sig = g.sig;
        g.nb_cache = g.sel.NeededByAll(*d);
    }
    if (d && (g.foot_ver != g.sel.Version() || g.foot_sig != g.sig)) {  // the footer and the "to pick" count, per selection change
        g.foot_ver = g.sel.Version();
        g.foot_sig = g.sig;
        g.foot_text = SyncFooterSummary(g.sel.Footer(*d), &g.foot_full);
        g.foot_pick = 0;
        for (const SyncChange& c : d->changes)
            if (c.side == SyncSide::Both && !g.sel.IsOn(c.key)) ++g.foot_pick;
    }
    DrawTitle(lib);
    const float footer_h = std::max(ui::LargeH() + ui::Dp(16.0f), ui::FontSmall()->FontSize * 3.0f + ui::Dp(10.0f));  // buttons, or 2 lines + "N to pick"
    DrawSub(info);
    ImGui::Dummy(ImVec2(0.0f, ui::Dp(2.0f)));
    Ctx c;
    c.d = d;
    c.store = &store;
    c.lib = lib;
    if (d && !d->changes.empty()) {
        const float x0 = ImGui::GetCursorScreenPos().x;
        const float w = std::max(1.0f, ImGui::GetContentRegionAvail().x - g.body_sb);
        DrawHeader(lib, g.model, *d, MakeCols(x0, w));
    }
    const float body_h = std::max(ui::Dp(60.0f), ImGui::GetContentRegionAvail().y - footer_h - ui::Dp(4.0f));
    if (ImGui::BeginChild("##sfbody", ImVec2(0.0f, body_h), ImGuiChildFlags_None)) {
        if (!d) {
            ui::ToneText(info.bad ? "Could not read the global library just now: it is read again when REAPER comes back to the front."
                                  : "Reading the global library...",
                         ui::kMuted, true);
        } else if (!d->changes.empty()) {
            const Cols cols = MakeCols(ImGui::GetCursorScreenPos().x, std::max(1.0f, ImGui::GetContentRegionAvail().x));
            g.body_sb = ImGui::GetScrollMaxY() > 0.0f ? ImGui::GetStyle().ScrollbarSize : 0.0f;
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));  // rows are exactly 24 px apart
            for (size_t gi = 0; gi < g.model.groups.size(); ++gi) {
                DrawGroupRow(c, g.model, static_cast<int>(gi), cols);
                if (!IsOpen(g.model.groups[gi].id)) continue;
                for (int ri : g.model.groups[gi].rows) DrawChangeRow(c, g.model, ri, cols);
            }
            ImGui::PopStyleVar();
            g.scroll_to.clear();  // used (or its group is not in this diff): it must not fire on a later rebuild
        } else {
            ui::ToneText("Nothing changed on either side.", ui::kMuted, true);
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
        g.nb_cache.clear();
        g.sel.Clear();
        g.fold.clear();
        g.open_row.clear();
        g.model = Model();
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
    if (!focus_node.empty()) {  // the container's own group is unfolded and scrolled to
        g.fold[focus_node] = true;
        g.scroll_to = focus_node;
    }
}

bool SyncWithFileIsOpen()
{
    return g.open;
}

bool SyncWithFileIsOpenFor(const std::string& lib_uid)
{
    return g.open && g.lib == lib_uid;
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
