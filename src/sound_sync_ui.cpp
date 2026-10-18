// SPDX-License-Identifier: MIT
//
// See sound_sync_ui.h.

#include "sound_sync_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

#include "library_tree_ui.h"
#include "library_view_ui.h"  // ShowLibraryToast
#include "reaper_item_read.h"  // ItemGuidOf
#include "sound_populate.h"   // SyncPlacedSounds
#include "sound_project.h"
#include "sound_record.h"
#include "sound_sync.h"
#include "sound_sync_plan.h"
#include "ui_flow.h"
#include "ui_theme.h"

namespace rav {

namespace {

constexpr const char* kSyncGlyph = "\xE2\x86\xBB";  // ↻
constexpr const char* kOkGlyph = "\xE2\x9C\x93";    // ✓
constexpr const char* kWhereId = "##syncwhere";

struct HandRow {
    std::string      owner, name, line, key;
    std::vector<std::string> items;
    bool             over = false;
};

struct Dialog {
    bool                     want_open = false;  // OpenPopup on the next DrawSyncDialogs
    int                      step = -1;          // -1 closed, 0 where, 1 hand edits
    std::string              lib, node;          // node "" = every change of the library
    std::set<std::string>    on;                 // the checked animation items
    bool                     seeded = false;     // `on` filled from the first lines
    std::vector<std::string> chosen;
    std::vector<HandRow>     hand;
    bool                     dna = false;
    bool                     close = false;
};
Dialog g;

// A gesture picked in a dialog, run once the dialogs are drawn.
struct Pending {
    bool              set = false;
    SyncPlacedRequest rq;
};
Pending g_run;

// The GUIDs (upper case) of the items selected in REAPER now.
std::set<std::string> SelectedInReaper()
{
    std::set<std::string> out;
    for (int i = 0, n = CountSelectedMediaItems(nullptr); i < n; ++i)
        if (MediaItem* it = GetSelectedMediaItem(nullptr, i)) {
            std::string g = ItemGuidOf(it);
            for (char& c : g)
                if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
            out.insert(g);
        }
    return out;
}

bool IsSelectedNow(const std::set<std::string>& sel, std::string guid)
{
    for (char& c : guid)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    return sel.count(guid) > 0;
}

// The ↻ marks of library `lib`, worked out once a frame (every row of the tree asks).
const std::map<std::string, int>& MarksNow(const SyncState& st, const std::string& lib)
{
    static int                        frame = -1;
    static std::string                for_lib;
    static std::map<std::string, int> marks;
    if (frame != ImGui::GetFrameCount() || for_lib != lib) {
        frame = ImGui::GetFrameCount();
        for_lib = lib;
        marks = SyncMarksOf(st, lib);
    }
    return marks;
}

ImU32 WarnSoft(bool hot) { return ui::WithAlpha(ui::kWarn, hot ? 0x33 : 0x1A); }

const SoundLibrary* LibOf(const std::string& uid) { return FindLibrary(GetSoundStore(nullptr), uid); }

std::string PathOf(const std::string& lib_uid, const std::string& node)
{
    const SoundLibrary* lib = LibOf(lib_uid);
    return lib ? SyncNodePath(GetSoundStore(nullptr), *lib, node) : node;
}

// The behind nodes the dialog covers.
std::vector<std::string> ScopeNodes(const SyncState& st)
{
    if (!g.node.empty()) return {g.node};
    std::vector<std::string> out;
    for (const auto& kv : SyncMarksOf(st, g.lib)) out.push_back(kv.first);
    return out;
}

// The chips: every behind node of the sounds in scope (a node-scoped Sync applies the whole request on their paths), the
// clicked node first.
std::vector<std::string> ChipNodes(const SyncState& st)
{
    std::vector<std::string> out;
    if (!g.node.empty()) out.push_back(g.node);
    for (const SyncSound& s : st.sounds) {
        if (!SyncInScope(s, g.lib, g.node)) continue;
        for (const std::string& b : s.behind)
            if (std::find(out.begin(), out.end(), b) == out.end()) out.push_back(b);
    }
    return out;
}

std::string ChipText(const SyncState& st, const std::string& node)
{
    std::string t = PathOf(g.lib, node);
    const auto  w = st.what.find(node);
    if (w != st.what.end() && !w->second.empty()) t += " \xC2\xB7 " + w->second;
    if (st.redraws.count(node)) t += " \xC2\xB7 redraws the sounds that played it";
    return t;
}

void Close()
{
    g.step = -1;
    g.close = false;
    g.hand.clear();
    ImGui::CloseCurrentPopup();
}

// The title row: its text (semibold, ellipsized) on the card's width.
void Title(const std::string& text)
{
    ImGui::PushFont(ui::FontSemibold());
    ui::FitText(text, ui::kText, ImGui::GetContentRegionAvail().x);
    ImGui::PopFont();
}

// What a Sync of the chosen items does without asking (no hand edit, or the project's rule says): run it.
void Run(const SyncState& st, const std::vector<std::string>& owners, const std::map<std::string, bool>& overwrite, bool remember,
         HandEditRule rule)
{
    g_run.set = true;
    g_run.rq = SyncPlacedRequest{};
    g_run.rq.owners = owners;
    g_run.rq.scope = ScopeNodes(st);
    g_run.rq.overwrite = overwrite;
    g_run.rq.remember = remember;
    g_run.rq.rule = rule;
}

// Apply to `owners`: the hand dialog when some chosen sound was edited by hand and the rule asks, else the Sync.
void Apply(const SyncState& st, const std::vector<std::string>& owners)
{
    g.chosen = owners;
    const std::set<std::string>         set(owners.begin(), owners.end());
    const std::vector<const SyncSound*> hs = SyncHandSounds(st, g.lib, g.node, set);
    const HandEditRule                  rule = HandEditRuleOf(GetSoundStore(nullptr));
    if (!hs.empty() && rule == HandEditRule::Ask) {
        g.hand.clear();
        std::map<std::string, std::string> owner_name;
        for (const SyncOwner& o : st.owners) owner_name[o.guid] = o.name;
        for (const SyncSound* s : hs) {
            HandRow r;
            r.owner = s->owner;
            r.name = s->name.empty() ? PathOf(g.lib, s->node) : s->name;
            char at[48];
            std::snprintf(at, sizeof at, " at %.2f s", s->t);
            r.line = owner_name[s->owner] + at;
            r.key = s->hand.text;
            r.items = s->items;
            r.line += "\x1f" + PathOf(g.lib, s->node);  // split when drawn: <item at t> · <what> · <path>
            g.hand.push_back(r);
        }
        g.dna = false;
        g.step = 1;
        return;
    }
    std::map<std::string, bool> over;
    for (const SyncSound* s : hs)
        for (const std::string& it : s->items) over[it] = rule == HandEditRule::Overwrite;
    Run(st, owners, over, false, rule);
    g.close = true;
}

void DrawWhere(const SyncState& st)
{
    const std::vector<SyncItemLine> lines = SyncItemLines(st, g.lib, g.node);
    if (lines.empty()) {  // synced meanwhile, or undone
        Close();
        return;
    }
    if (!g.seeded) {
        g.on.clear();
        for (const SyncItemLine& l : lines) g.on.insert(l.owner);
        g.seeded = true;
    }
    const std::set<std::string>    sel_now = SelectedInReaper();
    const float                    wrap = ImGui::GetContentRegionAvail().x;
    std::string title = "Sync placed sounds";
    if (!g.node.empty() && SyncMarksOf(st, g.lib).size() > 1) title += " \xC2\xB7 " + PathOf(g.lib, g.node);
    Title(title);
    ui::ToneText("Each placed sound keeps its variation, track and position. RAV updates what changed in the library.", ui::kMuted,
                 true, wrap);
    // The changes, one chip each (the redrawing ones amber).
    {
        bool first = true;
        for (const std::string& node : ChipNodes(st)) {
            const std::string t = ChipText(st, node);
            const bool        redraw = st.redraws.count(node) > 0;
            const float       w = ui::TagWidth(t.c_str());
            if (w > wrap) {  // too long for a chip: a wrapped line
                ui::ToneText(t, redraw ? ui::kWarn : ui::kMuted, true, wrap);
                first = true;
                continue;
            }
            if (!first) ui::SameLineIfFits(w);
            first = false;
            ui::Tag(t.c_str(), redraw ? ui::TagTone::Warn : ui::TagTone::Neutral);
        }
    }
    ImGui::Dummy(ImVec2(1.0f, ui::Dp(2.0f)));
    // The animation items, by track.
    const float list_h = std::min(ui::Dp(320.0f), std::max(ui::Dp(80.0f), lines.size() * ui::Dp(44.0f) + ui::Dp(40.0f)));
    if (ImGui::BeginChild("##syncitems", ImVec2(0.0f, list_h), ImGuiChildFlags_Borders)) {
        std::string track;
        bool        first = true;
        for (size_t i = 0; i < lines.size(); ++i) {
            const SyncItemLine& l = lines[i];
            ImGui::PushID(l.owner.c_str());
            if (first || l.track != track) {
                track = l.track;
                first = false;
                ImGui::PushID(("trk:" + track).c_str());
                int on = 0, all = 0;
                for (const SyncItemLine& x : lines)
                    if (x.track == track) {
                        ++all;
                        on += g.on.count(x.owner) ? 1 : 0;
                    }
                const int state = on == 0 ? 0 : on == all ? 1 : 2;
                bool      flip = ui::TriCheckbox("##trk", state);
                ui::ItemTipText("Every item of this track, or none");
                ImGui::SameLine();
                ui::GroupCaption(track.empty() ? "(unnamed track)" : track.c_str());
                ImGui::SameLine();
                flip = ui::LinkButton("all / none##trk") || flip;
                if (flip)
                    for (const SyncItemLine& x : lines)
                        if (x.track == track) {
                            if (state == 1) g.on.erase(x.owner);
                            else g.on.insert(x.owner);
                        }
                ImGui::PopID();
            }
            bool checked = g.on.count(l.owner) > 0;
            if (ui::Checkbox("##item", &checked)) {
                if (checked) g.on.insert(l.owner);
                else g.on.erase(l.owner);
            }
            ImGui::SameLine();
            const std::string count = SyncPlural(l.sounds, "sound");
            const float       cw = ImGui::CalcTextSize(count.c_str()).x;
            const bool        selected = IsSelectedNow(sel_now, l.owner);
            const float       tagw = selected ? ui::SmallTagWidth("selected in REAPER") + ui::Dp(6.0f) : 0.0f;
            const float       room = std::max(ui::Dp(40.0f), ImGui::GetContentRegionAvail().x - cw - tagw - ui::Dp(8.0f));
            ui::FitText(l.name.empty() ? std::string("(unnamed item)") : l.name, ui::kText, room);
            if (selected) {
                ImGui::SameLine(0.0f, ui::Dp(6.0f));
                ui::SmallTag("selected in REAPER");
            }
            ImGui::SameLine(0.0f, ui::Dp(8.0f));
            ui::ToneText(count, ui::kMuted);
            std::string nodes;
            for (const std::string& n : l.nodes) nodes += (nodes.empty() ? "" : " \xC2\xB7 ") + PathOf(g.lib, n);
            ImGui::Indent(ui::Dp(20.0f));
            ImGui::PushFont(ui::FontSmall());
            ui::ToneText(nodes, ui::kMuted, true);
            if (l.hand > 0) ui::ToneText(std::to_string(l.hand) + " edited by hand", ui::kWarn, true);
            ImGui::PopFont();
            ImGui::Unindent(ui::Dp(20.0f));
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    // The footer: the choice helpers, then Cancel / Apply to N items / Apply to all.
    size_t n_on = 0;
    for (const SyncItemLine& l : lines) n_on += g.on.count(l.owner) ? 1 : 0;
    if (ui::SolidButton("Selected in REAPER##syncsel")) {
        g.on.clear();
        for (const SyncItemLine& l : lines)
            if (IsSelectedNow(sel_now, l.owner)) g.on.insert(l.owner);
    }
    ui::ItemTipText("Check the animation items selected in REAPER");
    ImGui::SameLine();
    const bool all_on = n_on == lines.size();
    if (ui::SolidButton(all_on ? "None##syncallnone" : "All##syncallnone")) {
        g.on.clear();
        if (!all_on)
            for (const SyncItemLine& l : lines) g.on.insert(l.owner);
    }
    const std::string apply_n =
        SyncApplyCheckedEnabled(n_on, lines.size()) ? "Apply to " + SyncPlural(static_cast<int>(n_on), "item") + "##syncsome"
                                                    : std::string("Apply to checked##syncsome");
    const float bw = ui::ButtonWidth("Cancel##synccancel") + ui::ButtonWidth(apply_n.c_str()) +
                     ui::ButtonWidth("Apply to all##syncall") + 2.0f * ImGui::GetStyle().ItemSpacing.x;
    if (!ui::SameLineIfFits(bw)) ImGui::Dummy(ImVec2(0.0f, 0.0f));
    else ImGui::SetCursorScreenPos(ImVec2(std::max(ImGui::GetCursorScreenPos().x, ui::LineRight() - bw), ImGui::GetCursorScreenPos().y));
    if (ui::SolidButton("Cancel##synccancel")) g.close = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(!SyncApplyCheckedEnabled(n_on, lines.size()));
    if (ui::SolidButton(apply_n.c_str())) {
        std::vector<std::string> owners;
        for (const SyncItemLine& l : lines)
            if (g.on.count(l.owner)) owners.push_back(l.owner);
        Apply(st, owners);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ui::PrimaryButton("Apply to all##syncall")) {
        std::vector<std::string> owners;
        for (const SyncItemLine& l : lines) owners.push_back(l.owner);
        Apply(st, owners);
    }
}

void DrawHand(const SyncState& st)
{
    if (g.hand.empty()) {
        g.step = 0;
        return;
    }
    const float wrap = ImGui::GetContentRegionAvail().x;
    Title(SyncPlural(static_cast<int>(g.hand.size()), "placed sound") + " edited by hand");
    ui::ToneText("You changed these in REAPER after Populate. Keep leaves your change and updates the rest; Overwrite puts the "
                 "library's settings back on them.",
                 ui::kMuted, true, wrap);
    static const char* const kSeg[2] = {"Keep", "Overwrite"};
    bool all_keep = true, all_over = true;
    for (const HandRow& r : g.hand) {
        all_keep = all_keep && !r.over;
        all_over = all_over && r.over;
    }
    const float list_h = std::min(ui::Dp(320.0f), std::max(ui::Dp(80.0f), g.hand.size() * ui::Dp(44.0f) + ui::Dp(44.0f)));
    if (ImGui::BeginChild("##synchandlist", ImVec2(0.0f, list_h), ImGuiChildFlags_Borders)) {
        // All of them.
        {
            const float sw = ui::ButtonWidth("Keep") + ui::ButtonWidth("Overwrite") + ui::Dp(4.0f);
            ui::ToneText("All of them", ui::kMuted);
            ImGui::SameLine();
            ImGui::SetCursorScreenPos(ImVec2(std::max(ImGui::GetCursorScreenPos().x, ui::LineRight() - sw), ImGui::GetCursorScreenPos().y));
            int sel = all_keep ? 0 : all_over ? 1 : -1;
            if (ui::Segmented("##syncallseg", kSeg, 2, &sel))
                for (HandRow& r : g.hand) r.over = sel == 1;
            ImGui::Separator();
        }
        for (size_t i = 0; i < g.hand.size(); ++i) {
            HandRow& r = g.hand[i];
            ImGui::PushID(static_cast<int>(i));
            const float sw = ui::ButtonWidth("Keep") + ui::ButtonWidth("Overwrite") + ui::Dp(4.0f);
            const float x0 = ImGui::GetCursorScreenPos().x;
            const float y0 = ImGui::GetCursorScreenPos().y;
            const float text_w = std::max(ui::Dp(60.0f), ImGui::GetContentRegionAvail().x - sw - ui::Dp(10.0f));
            ImGui::BeginGroup();
            ImGui::PushTextWrapPos(x0 + text_w);
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::PopTextWrapPos();
            const size_t      cut = r.line.find('\x1f');
            const std::string where = cut == std::string::npos ? r.line : r.line.substr(0, cut);
            const std::string path = cut == std::string::npos ? std::string() : r.line.substr(cut + 1);
            ImGui::PushFont(ui::FontSmall());
            ui::ToneText(where + " \xC2\xB7 " + r.key + (path.empty() ? std::string() : " \xC2\xB7 " + path), ui::kMuted, true, text_w);
            ImGui::PopFont();
            ImGui::EndGroup();
            const float y1 = ImGui::GetItemRectMax().y;
            ImGui::SetCursorScreenPos(ImVec2(x0 + text_w + ui::Dp(10.0f), y0));
            int sel = r.over ? 1 : 0;
            if (ui::Segmented("##seg", kSeg, 2, &sel)) r.over = sel == 1;
            ImGui::SetCursorScreenPos(ImVec2(x0, std::max(y1, ImGui::GetItemRectMax().y) + ui::Dp(4.0f)));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    std::vector<char> overs;
    for (const HandRow& r : g.hand) overs.push_back(r.over ? 1 : 0);
    const bool can_remember = SyncRememberEnabled(overs);
    if (!can_remember) g.dna = false;
    ImGui::BeginDisabled(!can_remember);
    ui::Checkbox(can_remember ? "Don't ask again##syncdna" : "Don't ask again (choices differ)##syncdna", &g.dna);
    ImGui::EndDisabled();
    ui::ItemTipText(can_remember ? "Sets \xE2\x98\xB0 \xE2\x80\xBA Tagging view \xE2\x80\xBA Sounds \xE2\x80\xBA Hand-edited placed sounds"
                                 : "Only one rule can be remembered: pick Keep or Overwrite for all of them");
    const float bw = ui::ButtonWidth("Back##syncback") + ui::ButtonWidth("Sync##syncgo") + ImGui::GetStyle().ItemSpacing.x;
    if (!ui::SameLineIfFits(bw)) ImGui::Dummy(ImVec2(0.0f, 0.0f));
    else ImGui::SetCursorScreenPos(ImVec2(std::max(ImGui::GetCursorScreenPos().x, ui::LineRight() - bw), ImGui::GetCursorScreenPos().y));
    if (ui::SolidButton("Back##syncback")) {
        g.step = 0;
        return;
    }
    ImGui::SameLine();
    if (ui::PrimaryButton("Sync##syncgo")) {
        std::map<std::string, bool> over;
        for (const HandRow& r : g.hand)
            for (const std::string& it : r.items) over[it] = r.over;
        const bool remember = g.dna && can_remember;
        Run(st, g.chosen, over, remember, all_over ? HandEditRule::Overwrite : HandEditRule::Keep);
        g.close = true;
    }
}

}  // namespace

void MergeSyncTreeHooks()
{
    const LibraryTreeHooks& h = GetLibraryTreeHooks();
    if (h.sync_card && h.sync_mark) return;
    LibraryTreeHooks m = h;
    m.sync_card = [](const std::string& lib) { DrawPlacedSoundsCard(lib); };
    m.sync_mark = [](const std::string& lib, const std::string& node, float right_x, float cy) {
        return DrawSyncMark(lib, node, right_x, cy);
    };
    SetLibraryTreeHooks(m);
}

void DrawPlacedSoundsCard(const std::string& lib)
{
    const SyncState& st = PlacedSyncState();
    const SyncCard   c = SyncCardOf(st, lib);
    if (c.sounds == 0 && !st.libs_placed.count(lib)) return;  // nothing placed from it yet
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (c.sounds == 0) {
        // Every placed sound matches: one green line (the card folded).
        ui::Card k;
        ui::BeginCard(k, ui::Dp(4.0f));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  hh = ui::SectionHeaderH();
        ImGui::Dummy(ImVec2(std::max(1.0f, k.w - 2.0f * ui::Dp(8.0f)), hh));
        ImFont*     tf = ui::FontTitle();
        const float ty = std::floor(p.y + (hh - tf->FontSize) * 0.5f);
        ui::AddTextIn(dl, tf, ImVec2(p.x, ty), ui::kOk, kOkGlyph);
        const float gx = p.x + ui::TextSizeIn(tf, kOkGlyph).x + ui::Dp(6.0f);
        ImGui::PushFont(tf);
        ui::EllipsisTextTip(dl, ImVec2(gx, ty), p.x + k.w - 2.0f * ui::Dp(8.0f), ui::kText, "Placed sounds match the library");
        ImGui::PopFont();
        ui::EndCard(k, ui::kSurface, ui::kStroke);
        return;
    }
    const char*     kSync = "Sync\xE2\x80\xA6##syncall";
    ui::SectionOpts o;
    o.foldable = false;
    o.right_w = ui::ButtonWidth(kSync);
    ui::Section sec;
    const bool  open = ui::BeginSection(sec, "\xE2\x86\xBB Placed sounds##synccard", o);
    {  // the ↻ in amber over the title's own
        ImFont*     tf = ui::FontTitle();
        const float hh = sec.head_max.y - sec.head_min.y;
        ui::AddTextIn(dl, tf, ImVec2(sec.head_min.x, std::floor(sec.head_min.y + (hh - tf->FontSize) * 0.5f)), ui::kWarn, kSyncGlyph);
    }
    ui::BeginSectionHeaderRight(sec);
    if (ui::PrimaryButton(kSync)) OpenSyncDialog(lib, "");
    ui::ItemTipText("Choose the animation items to update");
    ui::EndSectionHeaderRight(sec);
    if (open) ui::ToneText(SyncCardText(c), ui::kMuted, true);
    const ImVec2 a = sec.card.p0;
    const float  w = sec.card.w;
    ui::EndSection(sec);
    // Its amber edge (the mockup's .sync): over the card's own.
    const float bottom = ImGui::GetItemRectMin().y;
    dl->AddRect(a, ImVec2(a.x + w, bottom), ui::kWarnLine, ui::kRadiusLg);
}

float DrawSyncMark(const std::string& lib, const std::string& node, float right_x, float cy)
{
    const SyncState&                  st = PlacedSyncState();
    const std::map<std::string, int>& marks = MarksNow(st, lib);
    const auto                        hit = marks.find(node);
    if (hit == marks.end() || hit->second <= 0) return 0.0f;
    char text[32];
    std::snprintf(text, sizeof text, "%s %d", kSyncGlyph, hit->second);
    ImFont*      f = ui::FontSmall();
    const float  h = ui::Dp(18.0f);
    const float  w = ui::TextSizeIn(f, text).x + ui::Dp(12.0f);
    const ImVec2 a(right_x - w, std::floor(cy - h * 0.5f));
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(node.c_str());
    const bool clicked = ImGui::InvisibleButton("##syncmark", ImVec2(w, h));
    const bool hot = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), WarnSoft(hot), h * 0.5f);
    dl->AddRect(a, ImVec2(a.x + w, a.y + h), ui::kWarnLine, h * 0.5f);
    ui::AddTextIn(dl, f, ImVec2(a.x + ui::Dp(6.0f), std::floor(a.y + (h - f->FontSize) * 0.5f)), ui::kWarn, text);
    const auto        what = st.what.find(node);
    const std::string tip = "Changed since placed: " + (what == st.what.end() ? std::string("settings") : what->second) +
                            " \xC2\xB7 " + SyncPlural(hit->second, "placed sound") + ". Click to sync this node.";
    ui::ItemTipText(tip.c_str());
    if (clicked) OpenSyncDialog(lib, node);
    return w;
}

void DrawSyncPendingLine(const std::string& lib, const std::string& node)
{
    const SyncState&                  st = PlacedSyncState();
    const std::map<std::string, int>& marks = MarksNow(st, lib);
    const auto                        hit = marks.find(node);
    if (hit == marks.end() || hit->second <= 0) return;
    const auto        what = st.what.find(node);
    const std::string text = std::string(kSyncGlyph) + " Not yet on " + SyncPlural(hit->second, "placed sound") + ": " +
                             (what == st.what.end() ? std::string("settings changed") : what->second);
    ImGui::Dummy(ImVec2(1.0f, ui::Dp(2.0f)));
    ui::ToneText(text, ui::kWarn, true);
    const char* kBtn = "Sync this node\xE2\x80\xA6##syncnode";
    ui::SameLineIfFits(ui::ButtonWidth(kBtn));
    if (ui::SolidButton(kBtn)) OpenSyncDialog(lib, node);
    ui::ItemTipText("Choose the animation items to update");
}

void OpenSyncDialog(const std::string& lib, const std::string& node)
{
    g = Dialog{};
    g.lib = lib;
    g.node = node;
    g.step = 0;
    g.want_open = true;
}

void DrawSyncDialogs()
{
    if (g.want_open) {
        g.want_open = false;
        ImGui::OpenPopup(kWhereId);
    }
    if (g.step >= 0 && !ImGui::IsPopupOpen(kWhereId)) g.step = -1;  // closed from outside
    if (ui::BeginModalCard(kWhereId, 0, ui::Dp(600.0f))) {
        const SyncState& st = PlacedSyncState();
        if (g.step < 0 || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            Close();
        } else {
            ImGui::PushID(g.step);
            if (g.step == 1) DrawHand(st);
            else DrawWhere(st);
            ImGui::PopID();
            if (g.close) Close();
        }
        ui::EndModalCard();
    }
    if (!g_run.set) return;
    const SyncPlacedRequest rq = g_run.rq;
    g_run = Pending{};
    SyncCounts        c;
    const SoundReport rep = SyncPlacedSounds(rq, &c);
    InvalidatePlacedSyncState();
    for (const ReportLine& l : rep.lines)
        if (l.kind == ReportKind::Blocked) {  // an error: said, never the success text
            ShowLibraryToast("Sync placed sounds: " + l.text, true, "RAV: Sync placed sounds");
            return;
        }
    ShowLibraryToast(SyncToastText(c, rq.remember), true, "RAV: Sync placed sounds");
}

}  // namespace rav

#endif  // _WIN32
