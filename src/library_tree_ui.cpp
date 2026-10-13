// SPDX-License-Identifier: MIT
//
// See library_tree_ui.h. Every gesture here is one ModifySoundStore undo point ("RAV: <gesture>")
// followed by a toast offering Undo; the edits themselves are library_tree_edit.h (pure,
// host-tested).

#include "library_tree_ui.h"

#include "library_icons.h"

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include <imgui_internal.h>  // the toolbar well narrows the work rect; the empty-library box is a drop target

#include "library_user_ui.h"
#include "library_view_ui.h"
#include "picked_gestures.h"
#include "reaper_api.h"
#include "item_rules.h"  // RulesResourceRoot, GetCustomRoles (13-23 role maps)
#include "sound_assign.h"
#include "sound_place.h"
#include "sound_project.h"
#include "sound_record.h"
#include "sound_entries.h"  // ToggleGroupRole (13-23 role maps)
#include "sound_switch.h"
#include "ui_flow.h"
#include "ui_flow_math.h"

namespace rav {
namespace {

// 13-7a (audit C3.3): the mockup's .trow: 4 px in, 18 px per level (ui::Dp of these).
constexpr float kIndent = 18.0f;
constexpr float kRowPad = 4.0f;
constexpr double kAutoOpenSeconds = 0.6;
constexpr const char kRowsPayload[] = "RAV_LIB_ROWS";

enum class RenameKind { None, Library, Group, Value, Row };

struct TreeState {
    std::string              lib;
    std::vector<LibraryRow>  sel;
    LibraryRow               anchor;
    // 13-17d: the row an arrow key moved to, scrolled into view when drawn.
    LibraryRow               scroll_to;
    bool                     has_scroll_to = false;
    // 13-17b: the tree's height on the last full frame (kept on a gesture's frame, so the scroll is
    // not clamped), and the shown library's node uids last frame (a node not in it is new).
    float                    rows_h = 0.0f;
    std::string              known_lib;
    std::set<std::string>    known_nodes;
    // 13-21: this frame's library gaps (node uid -> its reasons), each row's amber "!".
    std::map<std::string, std::string> gaps;
    std::vector<LibraryGap>            hidden_gaps;  // 13-21: gaps whose node no row shows (folded / searched out)
    bool                     has_anchor = false;
    std::set<std::string>    closed;  // the containers the user folded (every container starts open)
    std::vector<TreeRow>     rows;  // last frame's visible rows
    // Inline rename.
    RenameKind               rename = RenameKind::None;
    std::string              r_a, r_b;  // library / group (+ value) uids
    LibraryRow               r_row;
    char                     buf[256] = {};
    bool                     r_focus = false;
    // Cut / paste.
    std::string              clip_lib;
    std::vector<LibraryRow>  clip;
    // Drag.
    std::string              drag_lib;
    std::vector<LibraryRow>  drag_rows;
    std::string              hover_uid;
    double                   hover_since = 0.0;
    // Menus.
    bool                     open_row_menu = false;
    LibraryRow               menu_row;
    std::vector<LibraryRow>  menu_rows;
    LibraryTreeHooks         hooks;
    // Missing items: the project's item GUIDs (upper case), rebuilt on a project change.
    int                      guid_scc = -1;
    ReaProject*              guid_proj = nullptr;
    // Column 2 (toolbar + tree) had the keyboard focus last frame: the tree keys act only then.
    bool                     tree_focused = false;
    // Antho 2026-10-12: the Switch Groups row clicked last (a value when sg_vid is set): F2 and its right-click
    // menu rename it.
    std::string              sg_gid, sg_vid;
    bool                     sg_menu = false;
    // 13-18: the group default awaiting the confirm band (Switches on the group have their own default).
    std::string              sgd_gid, sgd_vid;
    std::set<std::string>    item_guids;
    // 13-7a (audit C2.3): each open group's dashed "+ value" field (by group uid); the group whose
    // field takes the keyboard again after a value was added (to type the next one).
    std::map<std::string, std::array<char, 128>> add_value;
    std::string              add_value_focus;
    // 13-7a (audit C3.1): the tree header's library name field (the name being typed).
    std::string              title_lib;
    char                     title[256] = {};
    // 13-17c: the tree's search field (session; kept across libraries, Esc clears it).
    char                     search[128] = {};
    // The row menu was open last frame (its row keeps its ⋯ lit).
    bool                     row_menu_open = false;
    // A gesture ran this frame: the store was rewritten, so every SoundLibrary / SoundNode
    // reference taken before it is stale; the drawing loops stop at once (next frame redraws).
    bool                     changed = false;
};
TreeState g;

// The 13-8 rule (picked_gestures.h), here too: a button, menu item, drop, rename field or key only
// picks its gesture, a closure holding copies of what it needs (uids, rows, names, the drop's
// data); the picks run once the part being drawn (Libraries, Switch Groups, the tree, the batch
// panel) is drawn, so no row, menu or loop still reads the store a gesture replaces
// (ModifySoundStore swaps it whole). Still one undo point per gesture.
PickedGestures g_picks;

// Picking also sets `changed`: the rest of the frame stops drawing the store, as before the picks.
void Later(std::function<void()> f)
{
    g_picks.Pick(std::move(f));
    g.changed = true;
}

std::string Upper(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

void RefreshItemGuids()
{
    const int scc = GetProjectStateChangeCount(nullptr);
    ReaProject* proj = EnumProjects(-1, nullptr, 0);  // a project tab switch rebuilds too
    if (scc == g.guid_scc && proj == g.guid_proj) return;
    g.guid_scc = scc;
    g.guid_proj = proj;
    g.item_guids.clear();
    const int n = CountMediaItems(nullptr);
    for (int i = 0; i < n; ++i) {
        char buf[64] = {};
        MediaItem* it = GetMediaItem(nullptr, i);
        if (it && GetSetMediaItemInfo_String(it, "GUID", buf, false)) g.item_guids.insert(Upper(buf));
    }
}

bool SoundMissing(const SoundNode& n)
{
    if (n.type != SoundNodeType::Sound) return false;
    if (n.sound.source.kind == SoundSourceKind::Item) {
        if (n.sound.source.item_guid.empty() || !g.item_guids.count(Upper(n.sound.source.item_guid))) return true;
    }
    return g.hooks.is_missing ? g.hooks.is_missing(n) : false;
}

const char* TypeWord(SoundNodeType t)
{
    switch (t) {
    case SoundNodeType::Sound:  return "Sound";
    case SoundNodeType::Random: return "Random";
    case SoundNodeType::Blend:  return "Blend";
    default:                    return "Switch";
    }
}

// 13-7 (audit A5): one ink for every container's icon (the mockup's #a8c2ff, .trow.sw .ti),
// muted for a Sound -- as TypeIconInk (library_icons.h).
ImU32 TypeColor(SoundNodeType t)
{
    return t == SoundNodeType::Sound ? ui::kMuted : ui::kAccentInk;
}

// One gesture = one undo point + a toast offering Undo.
bool Gesture(const std::string& name, const std::function<bool(SoundStore&)>& edit, const std::string& toast = {})
{
    const std::string desc = "RAV: " + name;
    if (!ModifySoundStore(nullptr, desc.c_str(), edit)) return false;
    g.changed = true;
    ShowLibraryToast(toast.empty() ? name : toast, true, desc);
    return true;
}

const SoundLibrary* CurrentLib()
{
    const SoundStore& store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, g.lib);
    if (!lib && !store.libraries.empty()) {
        g.lib = store.libraries.front().uid;
        g.sel.clear();
        lib = &store.libraries.front();
    }
    if (!lib) g.lib.clear();
    return lib;
}

bool IsSel(const LibraryRow& r) { return std::find(g.sel.begin(), g.sel.end(), r) != g.sel.end(); }

void SelectOnly(const LibraryRow& r)
{
    g.sel = {r};
    g.anchor = r;
    g.has_anchor = true;
}

// Opens every container above a node (so its row is visible).
void RevealNode(const SoundLibrary& lib, const std::string& uid)
{
    const SoundNode* n = FindNode(lib, uid);
    for (size_t guard = 0; n && !n->parent.empty() && guard < lib.nodes.size(); ++guard) {
        g.closed.erase(n->parent);
        n = FindNode(lib, n->parent);
    }
}

void StartRowRename(const SoundLibrary& lib, const LibraryRow& row)
{
    // A value row shows (and renames) its value's name; any other row its node's.
    std::string name;
    bool found = false;
    if (RowIsValue(lib, row)) {
        if (const SoundNode* sw = FindNode(lib, row.parent))
            if (const SwitchGroup* gr = FindGroup(GetSoundStore(nullptr), sw->group_uid))
                for (const SwitchValue& v : gr->values)
                    if (v.uid == row.key) {
                        name = v.name;
                        found = true;
                    }
    }
    if (!found) {
        const SoundNode* n = FindNode(lib, RowNode(lib, row));
        if (!n) return;
        name = n->name;
    }
    g.rename = RenameKind::Row;
    g.r_a = lib.uid;
    g.r_row = row;
    std::snprintf(g.buf, sizeof(g.buf), "%s", name.c_str());
    g.r_focus = true;
}

// The new nodes of an add: the first one is selected, its parent opened, its name ready to type.
void FocusNew(const std::string& lib_uid, const std::vector<std::string>& made, bool rename)
{
    const SoundLibrary* lib = FindLibrary(GetSoundStore(nullptr), lib_uid);
    if (!lib || made.empty()) return;
    RevealNode(*lib, made.front());
    const LibraryRow r = RowOfNode(*lib, made.front());
    SelectOnly(r);
    if (rename) StartRowRename(*lib, r);
}

// ---- Small drawing helpers ----------------------------------------------------------------------

// 13-7a (audit C3.3): a type icon, 16 px, centred in the square at p of side `box` (20 px: a
// container's .tib button face, framed -- raised fill, stroke-strong edge, r5 -- while hovered or
// selected; 16 px: a Sound's .ti). Containers in kAccentInk, a Sound muted (TypeColor).
void DrawTypeIcon(ImDrawList* dl, ImVec2 p, float box, SoundNodeType t, bool framed, ImU32 ink)
{
    if (framed) {
        dl->AddRectFilled(p, ImVec2(p.x + box, p.y + box), ui::kRaised, ui::kRadiusSm);
        dl->AddRect(p, ImVec2(p.x + box, p.y + box), ui::kStrokeStrong, ui::kRadiusSm);
    }
    const float is = std::min(box, ui::Dp(16.0f)), o = std::floor((box - is) * 0.5f);
    DrawNodeTypeGlyph(dl, ImVec2(p.x + o, p.y + o), is, t, ink);
}

// A small ghost button drawn over a row (the mockup's .more / .playb / .star): w x h at p, hover
// kHover r5; `on` keeps the hover look (playing). Returns true when clicked; *hovered as it is.
bool RowToolButton(const char* id, ImVec2 p, float w, float h, bool on, bool* hovered)
{
    ImGui::SetCursorScreenPos(p);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    if (hov || on)
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ui::kHover, ui::kRadiusSm);
    if (hovered) *hovered = hov;
    return clicked;
}

// `glyph` in `font` centred in [a, b].
void CentredGlyph(ImDrawList* dl, ImFont* font, ImVec2 a, ImVec2 b, ImU32 col, const char* glyph)
{
    const ImVec2 ts = ui::TextSizeIn(font, glyph);
    ui::AddTextIn(dl, font, ImVec2(std::floor((a.x + b.x - ts.x) * 0.5f), std::floor((a.y + b.y - ts.y) * 0.5f)), col, glyph);
}

// A library's colour (colour 0 = the accent): the theme's (13-15b moved it there, the assignment window draws it too).
using ui::LibDot;

// 13-7a: the layout helpers are the shared ones (ui_flow.h): rows of buttons wrap, labels and
// buttons shrink with an ellipsis, nothing is cut off by a narrow column.
using ui::ButtonWidth;
using ui::MenuButton;
using ui::MenuButtonWidth;
using ui::SameLineIfFits;

// An inline name field (13-7a: the mockup's .redit / .gedit, ui::RenameField, 22 px): 1 = commit,
// -1 = cancel, 0 = still typing.
int InlineName(float width)
{
    if (g.r_focus) {
        ImGui::SetKeyboardFocusHere();
        g.r_focus = false;
    }
    const bool enter = ui::RenameField("##rename", g.buf, sizeof(g.buf), std::max(ui::Dp(40.0f), width),
                                       ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (enter) return 1;
    if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape)) return -1;
    if (ImGui::IsItemDeactivated()) return ImGui::IsKeyPressed(ImGuiKey_Escape) ? -1 : 1;
    return 0;
}

void EndRename() { g.rename = RenameKind::None; }

// ---- Gestures on rows ---------------------------------------------------------------------------

// The REAPER items selected now as new Sounds (folder items skipped).
std::vector<NewNodeSpec> SelectedItemSounds()
{
    std::vector<NewNodeSpec> out;
    const int n = CountSelectedMediaItems(nullptr);
    for (int i = 0; i < n; ++i) {
        MediaItem* item = GetSelectedMediaItem(nullptr, i);
        if (!item || IsFolderItem(item)) continue;
        NewNodeSpec sp;
        sp.type = SoundNodeType::Sound;
        char buf[64] = {};
        if (GetSetMediaItemInfo_String(item, "GUID", buf, false)) sp.sound.source.item_guid = buf;
        sp.sound.source.kind = SoundSourceKind::Item;
        sp.sound.mode = DetectMode::Whole;
        {  // the item's gain, kept with the source (it outlives a conversion to a file)
            const double vol = GetMediaItemInfo_Value(item, "D_VOL");
            sp.sound.source.item_db = vol > 0.0 ? 20.0 * std::log10(vol) : -60.0;
        }
        if (MediaItem_Take* take = GetActiveTake(item))
            if (const char* nm = GetTakeName(take)) sp.name = nm;
        if (sp.name.empty()) sp.name = "Sound";
        out.push_back(sp);
    }
    if (out.empty()) {
        NewNodeSpec sp;
        sp.type = SoundNodeType::Sound;
        sp.name = "New Sound";
        out.push_back(sp);
    }
    return out;
}

// The rows a toolbar / menu add acts on (none = the root).
std::vector<LibraryRow> AddTargets()
{
    return g.sel;
}

// How many targets can take a "+ type", and the reason when some cannot.
int AddReadiness(const SoundLibrary& lib, SoundNodeType type, std::string* why)
{
    const SoundStore& store = GetSoundStore(nullptr);
    std::vector<LibraryRow> t = AddTargets();
    if (t.empty()) return CanAddAt(store, lib, nullptr, type, why) ? 1 : 0;
    int ok = 0;
    std::string first_why;
    for (const LibraryRow& r : t) {
        std::string w;
        if (CanAddAt(store, lib, &r, type, &w)) ++ok;
        else if (first_why.empty()) first_why = w;
    }
    if (why) {
        if (ok == 0) *why = first_why;
        else if (t.size() > 1)
            *why = "Adds one to each of the " + std::to_string(t.size()) + " selected rows" +
                   (ok < static_cast<int>(t.size()) ? " (" + std::to_string(t.size() - ok) + " cannot take it)" : "");
        else why->clear();
    }
    return ok;
}

void DoAdd(SoundNodeType type, const std::string& group_uid, bool new_group = false,
           const std::vector<LibraryRow>* rows_in = nullptr)
{
    const std::string lib_uid = g.lib;
    std::vector<LibraryRow> rows = rows_in ? *rows_in : AddTargets();
    std::vector<NewNodeSpec> specs;
    if (type == SoundNodeType::Sound) {
        specs = SelectedItemSounds();
    } else {
        NewNodeSpec sp;
        sp.type = type;
        sp.group_uid = group_uid;
        specs.push_back(sp);
    }
    std::vector<std::string> made;
    int done = 0;
    const std::string what = std::string("Add ") + TypeWord(type);
    const bool ok = Gesture(
        what,
        [&](SoundStore& s) {
            if (new_group) {
                std::string gid;
                AddGroup(s, "Group", &gid);
                for (NewNodeSpec& sp : specs) sp.group_uid = gid;
            }
            bool any = false;
            if (rows.empty()) {
                any = AddNodes(s, lib_uid, nullptr, specs, &made, nullptr);
                done = any ? 1 : 0;
            } else {
                for (const LibraryRow& r : rows) {
                    std::vector<std::string> m;
                    if (AddNodes(s, lib_uid, &r, specs, &m, nullptr)) {
                        any = true;
                        ++done;
                        made.insert(made.end(), m.begin(), m.end());
                    }
                }
            }
            return any;
        },
        what + (rows.size() > 1 ? " \xC2\xBB " + std::to_string(done) : std::string()));
    if (!ok) return;
    if (rows.size() <= 1) FocusNew(lib_uid, made, true);  // several rows: the selection stays
}

void DoRemove(const std::vector<LibraryRow>& rows)
{
    const std::string lib_uid = g.lib;
    if (Gesture("Remove", [&](SoundStore& s) { return RemoveRows(s, lib_uid, rows); })) g.sel.clear();
}

void DoDuplicate(const std::vector<LibraryRow>& rows)
{
    const std::string lib_uid = g.lib;
    std::vector<std::string> made;
    if (Gesture("Duplicate", [&](SoundStore& s) { return DuplicateRows(s, lib_uid, rows, &made); }) && made.size() == 1)
        FocusNew(lib_uid, made, false);
}

// Where a paste / Put into lands for a target row (none = the root).
DropTarget PasteTarget(const SoundLibrary& lib, const LibraryRow* row)
{
    DropTarget t;
    t.lib = lib.uid;
    if (!row) {
        t.row = {"", lib.root};
        t.mode = DropMode::Into;
        return t;
    }
    t.row = *row;
    if (RowIsValue(lib, *row)) {
        t.mode = DropMode::OntoValue;
        return t;
    }
    const SoundNode* n = FindNode(lib, RowNode(lib, *row));
    t.mode = (n && n->type != SoundNodeType::Sound) ? DropMode::Into : DropMode::After;
    return t;
}

void DoCut(const std::string& lib_uid, const std::vector<LibraryRow>& rows)
{
    g.clip_lib = lib_uid;
    g.clip = rows;
    ShowLibraryToast(std::to_string(rows.size()) + (rows.size() > 1 ? " rows cut" : " row cut") +
                         ": select where they go and press Ctrl+V", false);
}

void DoPaste(const std::string& lib_uid, const LibraryRow* row)
{
    if (g.clip.empty()) return;
    const SoundLibrary* lib = FindLibrary(GetSoundStore(nullptr), lib_uid);
    if (!lib) return;
    const DropTarget to = PasteTarget(*lib, row);  // `lib` is stale once the gesture rewrote the store
    const std::string src = g.clip_lib;
    const std::vector<LibraryRow> rows = g.clip;
    if (!CanDrop(GetSoundStore(nullptr), src, rows, to)) {
        ShowLibraryToast("Cannot paste there", false);
        return;
    }
    std::vector<std::string> moved;
    if (Gesture("Paste", [&](SoundStore& s) { return MoveRows(s, src, rows, to, &moved); })) {
        g.clip.clear();
        FocusNew(lib_uid, moved, false);
    }
}

void DoSetType(const std::vector<LibraryRow>& rows, SoundNodeType type, const std::string& group_uid)
{
    const std::string lib_uid = g.lib;
    Gesture(std::string("Make it ") + TypeWord(type),
            [&](SoundStore& s) { return SetRowsType(s, lib_uid, rows, type, group_uid); });
}

void DoWrap(const std::vector<LibraryRow>& rows, SoundNodeType type, const std::string& group_uid)
{
    const std::string lib_uid = g.lib;
    if (rows.size() > 1) {
        std::string made;
        if (Gesture(std::string("Group into ") + TypeWord(type),
                    [&](SoundStore& s) { return GroupRows(s, lib_uid, rows, type, group_uid, &made); }))
            FocusNew(lib_uid, {made}, false);
        return;
    }
    std::vector<std::string> made;
    if (Gesture(std::string("Wrap in ") + TypeWord(type),
                [&](SoundStore& s) { return WrapRows(s, lib_uid, rows, type, group_uid, &made); }))
        FocusNew(lib_uid, made, false);
}

// The batch "Switch on <group>": a toast says why when nothing changed (bug 3: never silent).
void BatchSwitchOn(const std::vector<LibraryRow>& rows, const std::string& group_uid)
{
    const std::string lib_uid = g.lib;
    if (Gesture("Make it Switch",
                [&](SoundStore& s) { return SetRowsType(s, lib_uid, rows, SoundNodeType::Switch, group_uid); }))
        return;
    const SoundStore& store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, lib_uid);
    const SwitchGroup* gr = FindGroup(store, group_uid);
    bool any_container = false;
    for (const LibraryRow& r : rows)
        if (lib && !RowTypeNode(*lib, r).empty()) any_container = true;
    ShowLibraryToast(!gr ? std::string("That Switch Group no longer exists")
                     : any_container ? "Already Switch on " + gr->name
                                     : std::string("Select containers (Random, Blend, Switch): sounds have no type to change"),
                     false);
}

void DoMakeDefault(const std::vector<LibraryRow>& rows)
{
    const std::string lib_uid = g.lib;
    Gesture("Make default", [&](SoundStore& s) { return MakeDefault(s, lib_uid, rows); });
}

void PlayRow(const SoundLibrary& lib, const LibraryRow& row)
{
    if (!g.hooks.play) return;
    const std::string n = RowNode(lib, row);
    if (!n.empty()) g.hooks.play(lib.uid, n);
}

// "Switch on ▸" items: one per group. Returns the picked group's uid ("" = none).
std::string GroupMenuItems(bool with_new_group, bool* new_group)
{
    std::string picked;
    const SoundStore& store = GetSoundStore(nullptr);
    for (const SwitchGroup& gr : store.groups) {
        ImGui::PushID(gr.uid.c_str());
        if (ImGui::MenuItem(gr.name.c_str())) picked = gr.uid;
        ImGui::PopID();
    }
    if (store.groups.empty() && !with_new_group) ImGui::TextDisabled("No Switch Group yet");
    if (with_new_group) {
        ImGui::Separator();
        if (ImGui::MenuItem("New group") && new_group) *new_group = true;
    }
    return picked;
}

// The type menu of a container: Random / Blend / Switch on ▸ group.
void TypeMenu(const std::vector<LibraryRow>& rows)
{
    if (ui::IconMenuItem(TypeIcon(SoundNodeType::Random), "Random")) Later([rows] { DoSetType(rows, SoundNodeType::Random, {}); });
    if (ui::IconMenuItem(TypeIcon(SoundNodeType::Blend), "Blend")) Later([rows] { DoSetType(rows, SoundNodeType::Blend, {}); });
    if (ui::IconBeginMenu(TypeIcon(SoundNodeType::Switch), "Switch on")) {
        const std::string gid = GroupMenuItems(false, nullptr);
        if (!gid.empty()) Later([rows, gid] { BatchSwitchOn(rows, gid); });
        ImGui::EndMenu();
    }
}

void WrapMenu(const std::vector<LibraryRow>& rows)
{
    if (ui::IconMenuItem(TypeIcon(SoundNodeType::Random), "Random")) Later([rows] { DoWrap(rows, SoundNodeType::Random, {}); });
    if (ui::IconMenuItem(TypeIcon(SoundNodeType::Blend), "Blend")) Later([rows] { DoWrap(rows, SoundNodeType::Blend, {}); });
    if (ui::IconBeginMenu(TypeIcon(SoundNodeType::Switch), "Switch on")) {
        const std::string gid = GroupMenuItems(false, nullptr);
        if (!gid.empty()) Later([rows, gid] { DoWrap(rows, SoundNodeType::Switch, gid); });
        ImGui::EndMenu();
    }
}

// ---- The row menu (right-click / ⋯) -------------------------------------------------------------

void RowMenu(const SoundLibrary& lib)
{
    if (!ui::BeginMenuPopup("##rowmenu")) return;
    const std::vector<LibraryRow> rows = g.menu_rows;
    const LibraryRow row = g.menu_row;
    const std::string node_uid = RowNode(lib, row);
    const SoundNode* n = FindNode(lib, node_uid);
    const bool is_value = RowIsValue(lib, row);
    const bool container = n && n->type != SoundNodeType::Sound;
    const bool is_switch = n && n->type == SoundNodeType::Switch;
    const bool has_node = n != nullptr;
    std::string value_group;  // the value row's group
    if (is_value)
        if (const SoundNode* sw = FindNode(lib, row.parent)) value_group = sw->group_uid;
    bool group_known = false;  // the value row names a value its group still has
    if (const SwitchGroup* vg = value_group.empty() ? nullptr : FindGroup(GetSoundStore(nullptr), value_group))
        for (const SwitchValue& v : vg->values)
            if (v.uid == row.key) group_known = true;
    const std::string lib_uid = lib.uid, root_uid = lib.root;

    // Greyed when none of the rows can take that type (the tooltip says why).
    auto can_add = [&](SoundNodeType t, std::string* why) {
        bool any = false;
        for (const LibraryRow& r : rows) {
            std::string w;
            if (CanAddAt(GetSoundStore(nullptr), lib, &r, t, &w)) any = true;
            else if (why && why->empty()) *why = w;
        }
        if (any && why) why->clear();
        return any;
    };
    if (ImGui::BeginMenu("Add child")) {
        std::string why;
        if (ui::IconMenuItem(TypeIcon(SoundNodeType::Sound), "Sound", nullptr, false, can_add(SoundNodeType::Sound, &why)))
            Later([rows] { DoAdd(SoundNodeType::Sound, {}, false, &rows); });
        ui::ItemTipText(why.c_str());
        why.clear();
        if (ui::IconMenuItem(TypeIcon(SoundNodeType::Random), "Random", nullptr, false, can_add(SoundNodeType::Random, &why)))
            Later([rows] { DoAdd(SoundNodeType::Random, {}, false, &rows); });
        ui::ItemTipText(why.c_str());
        why.clear();
        if (ui::IconMenuItem(TypeIcon(SoundNodeType::Blend), "Blend", nullptr, false, can_add(SoundNodeType::Blend, &why)))
            Later([rows] { DoAdd(SoundNodeType::Blend, {}, false, &rows); });
        ui::ItemTipText(why.c_str());
        if (can_add(SoundNodeType::Switch, nullptr) && ui::IconBeginMenu(TypeIcon(SoundNodeType::Switch), "Switch on")) {
            bool ng = false;
            const std::string gid = GroupMenuItems(true, &ng);
            if (!gid.empty() || ng) Later([rows, gid, ng] { DoAdd(SoundNodeType::Switch, gid, ng, &rows); });
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (container && ImGui::BeginMenu("Make it / Convert to")) {
        TypeMenu(rows);
        ImGui::EndMenu();
    }
    if (has_node && ImGui::BeginMenu(rows.size() > 1 ? "Group into" : "Wrap in")) {
        WrapMenu(rows);
        ImGui::EndMenu();
    }
    if (is_switch && ImGui::BeginMenu("Change switch group")) {
        const std::string gid = GroupMenuItems(false, nullptr);
        if (!gid.empty()) Later([rows, gid] { BatchSwitchOn(rows, gid); });
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Rename", "F2", false, (has_node || is_value) && !g.changed)) StartRowRename(lib, row);
    const bool movable = has_node && node_uid != root_uid;
    if (ImGui::MenuItem("Cut", "Ctrl+X", false, movable)) DoCut(lib_uid, rows);
    if (ImGui::MenuItem("Paste", "Ctrl+V", false, !g.clip.empty() && !g.changed)) Later([lib_uid, row] { DoPaste(lib_uid, &row); });
    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, movable)) Later([rows] { DoDuplicate(rows); });
    if (is_value) {
        ImGui::Separator();
        if (ImGui::MenuItem("Make default")) Later([rows] { DoMakeDefault(rows); });
        if (!node_uid.empty() && ImGui::MenuItem("Clear (plays the default)"))
            Later([lib_uid, row] { Gesture("Clear value", [&](SoundStore& s) { return ClearValue(s, lib_uid, row); }); });
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Remove", "Del", false, movable)) Later([rows] { DoRemove(rows); });
    if (is_value && group_known) {
        if (ImGui::MenuItem("Remove value from group")) {
            const std::string gid = value_group, vid = row.key;
            Later([gid, vid] { Gesture("Remove value", [&](SoundStore& s) { return DeleteValue(s, gid, vid); }); });
        }
    }
    ui::EndMenuPopup();
}

// ---- Drag and drop ------------------------------------------------------------------------------

std::string DragGhost(const SoundLibrary& lib, const std::vector<LibraryRow>& rows)
{
    if (rows.size() > 1) return std::to_string(rows.size()) + " rows";
    const SoundNode* n = rows.empty() ? nullptr : FindNode(lib, RowNode(lib, rows.front()));
    return n ? n->name : std::string("1 sound");
}

void RowDragSource(const SoundLibrary& lib, const TreeRow& r)
{
    if (r.node.empty() || r.node == lib.root) return;
    if (!ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) return;
    if (ImGui::GetDragDropPayload() == nullptr || !ImGui::GetDragDropPayload()->IsDataType(kRowsPayload)) {
        g.drag_lib = lib.uid;
        g.drag_rows = IsSel(r.row) ? g.sel : std::vector<LibraryRow>{r.row};
    }
    const int dummy = 1;
    ImGui::SetDragDropPayload(kRowsPayload, &dummy, sizeof(dummy));
    ImGui::TextUnformatted(DragGhost(lib, g.drag_rows).c_str());
    ImGui::EndDragDropSource();
}

// Bug 6: a Media Explorer drag (LibraryTreeHooks::media_payload_type, set by the media panel) onto
// a row or the empty space (the root): highlighted while over it, handed to media_drop on release.
// Called inside BeginDragDropTarget. The hooks are read now, so their set-up order does not matter.
void MediaDropOn(const std::string& lib_uid, const LibraryRow& row, ImVec2 p0, ImVec2 p1)
{
    const LibraryTreeHooks& h = g.hooks;
    if (h.media_payload_type.empty() || !h.media_drop) return;
    const ImGuiPayload* pl = ImGui::GetDragDropPayload();
    if (!pl || !pl->IsDataType(h.media_payload_type.c_str())) return;
    // 13-7a (audit C3.7): the mockup's .drop-in, accent-soft with a 2 px accent outline inside.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, ui::kAccentSoft, ui::kRadiusSm);
    dl->AddRect(ImVec2(p0.x + 1.0f, p0.y + 1.0f), ImVec2(p1.x - 1.0f, p1.y - 1.0f), ui::kAccent, ui::kRadiusSm, 0, 2.0f);
    if (const ImGuiPayload* mp = ImGui::AcceptDragDropPayload(h.media_payload_type.c_str(),
                                                              ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
        const auto        drop = h.media_drop;  // the hook may replace the hooks
        const char*       d = static_cast<const char*>(mp->Data);
        std::vector<char> data(d, d ? d + mp->DataSize : d);  // the payload lives only this frame
        Later([drop, lib_uid, row, data] {
            drop(lib_uid, row, data.data(), static_cast<int>(data.size()));
            g.changed = true;  // the hook may have rewritten the store
        });
    }
}

// Inside a drag-drop target over [p0, p1]: rows or a Media Explorer drag go into the root (a Switch
// root has no "into"), the area lit as the mockup's .drop-in while an acceptable drag is over it.
void RootDropOn(const SoundLibrary& lib, ImVec2 p0, ImVec2 p1)
{
    const LibraryRow root_row{"", lib.root};
    const ImGuiPayload* pl = ImGui::GetDragDropPayload();
    if (pl && pl->IsDataType(kRowsPayload)) {
        DropTarget to;
        to.lib = lib.uid;
        to.row = root_row;
        to.mode = DropMode::Into;
        if (!CanDrop(GetSoundStore(nullptr), g.drag_lib, g.drag_rows, to)) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
        } else {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, p1, ui::kAccentSoft, ui::kRadiusMd);
            dl->AddRect(ImVec2(p0.x + 1.0f, p0.y + 1.0f), ImVec2(p1.x - 1.0f, p1.y - 1.0f), ui::kAccent, ui::kRadiusMd, 0, 2.0f);
            if (ImGui::AcceptDragDropPayload(kRowsPayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                Later([src = g.drag_lib, rows = g.drag_rows, lib_uid = lib.uid, to] {
                    std::vector<std::string> moved;
                    if (Gesture("Move", [&](SoundStore& s) { return MoveRows(s, src, rows, to, &moved); }))
                        FocusNew(lib_uid, moved, false);
                });
                g.drag_rows.clear();
            }
        }
    } else {
        MediaDropOn(lib.uid, root_row, p0, p1);
    }
}

// Rows dragged onto the empty space below the rows: into the root.
void EmptySpaceDropTarget(const SoundLibrary& lib)
{
    const float avail = ImGui::GetContentRegionAvail().y;
    const float h = std::max(ui::RowH() * 2.0f, avail);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x), h));
    if (!ImGui::BeginDragDropTarget()) return;
    RootDropOn(lib, p0, ImGui::GetItemRectMax());
    ImGui::EndDragDropTarget();
}

void RowDropTarget(const SoundLibrary& lib, const TreeRow& r, ImVec2 p0, ImVec2 p1)
{
    if (!ImGui::BeginDragDropTarget()) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float my = ImGui::GetIO().MousePos.y, h = p1.y - p0.y;
    const float f = h > 0.0f ? (my - p0.y) / h : 0.5f;
    DropTarget to;
    to.lib = lib.uid;
    to.row = r.row;
    const SoundNode* n = r.node.empty() ? nullptr : FindNode(lib, r.node);
    const bool rnd_blend = n && (n->type == SoundNodeType::Random || n->type == SoundNodeType::Blend);
    if (r.kind == TreeRowKind::Value) to.mode = DropMode::OntoValue;
    else if (r.kind == TreeRowKind::Hint) to.mode = DropMode::Into;
    else if (r.row.parent.empty()) to.mode = DropMode::Into;  // the root row
    else if (rnd_blend && f > 0.25f && f < 0.75f) to.mode = DropMode::Into;
    else to.mode = f < 0.5f ? DropMode::Before : DropMode::After;

    // A closed folder opens after ~0.6 s of hover.
    if (n && n->type != SoundNodeType::Sound && !r.open) {
        if (g.hover_uid != n->uid) {
            g.hover_uid = n->uid;
            g.hover_since = ImGui::GetTime();
        } else if (ImGui::GetTime() - g.hover_since > kAutoOpenSeconds) {
            g.closed.erase(n->uid);
        }
    }

    const ImGuiPayload* pl = ImGui::GetDragDropPayload();
    if (pl && pl->IsDataType(kRowsPayload)) {
        const bool ok = CanDrop(GetSoundStore(nullptr), g.drag_lib, g.drag_rows, to);
        if (ok) {
            // 13-7a (audit C3.7): before / after = a 2 px accent line inside the row's edge; into =
            // accent-soft with a 2 px accent outline inside.
            if (to.mode == DropMode::Before || to.mode == DropMode::After) {
                const float y = to.mode == DropMode::Before ? p0.y + 1.0f : p1.y - 1.0f;
                dl->AddLine(ImVec2(p0.x + ui::Dp(kRowPad + r.depth * kIndent), y), ImVec2(p1.x, y), ui::kAccent, 2.0f);
            } else {
                dl->AddRectFilled(p0, p1, ui::kAccentSoft, ui::kRadiusSm);
                dl->AddRect(ImVec2(p0.x + 1.0f, p0.y + 1.0f), ImVec2(p1.x - 1.0f, p1.y - 1.0f), ui::kAccent, ui::kRadiusSm, 0, 2.0f);
            }
            if (ImGui::AcceptDragDropPayload(kRowsPayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                Later([src = g.drag_lib, rows = g.drag_rows, lib_uid = lib.uid, to] {
                    std::vector<std::string> moved;
                    if (!Gesture("Move", [&](SoundStore& s) { return MoveRows(s, src, rows, to, &moved); })) return;
                    if (const SoundLibrary* dl2 = FindLibrary(GetSoundStore(nullptr), lib_uid)) {
                        g.sel.clear();
                        for (const std::string& m : moved) {
                            RevealNode(*dl2, m);
                            g.sel.push_back(RowOfNode(*dl2, m));
                        }
                    }
                });
                g.drag_rows.clear();
            }
        } else {
            ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);  // the mockup's .drop-no
        }
    } else {
        MediaDropOn(lib.uid, r.row, p0, p1);
    }
    ImGui::EndDragDropTarget();
}

// ---- Toolbar ------------------------------------------------------------------------------------

void ToolbarAdd(const SoundLibrary& lib, SoundNodeType type, const char* label)
{
    std::string why;
    const int ok = AddReadiness(lib, type, &why);
    const ui::Icon icon = TypeIcon(type);  // the mock-up's .tbb: the type icon before the label
    if (type == SoundNodeType::Switch) {
        if (MenuButton(label, ok > 0, nullptr, &icon)) ImGui::OpenPopup("##tbswitch");
        ui::ItemTipText(why.c_str());
        if (ui::BeginMenuPopup("##tbswitch")) {
            ui::MenuCaption("Switch on");
            bool ng = false;
            const std::string gid = GroupMenuItems(true, &ng);
            if (!gid.empty() || ng) Later([gid, ng, rows = AddTargets()] { DoAdd(SoundNodeType::Switch, gid, ng, &rows); });
            ui::EndMenuPopup();
        }
        return;
    }
    if (ok == 0) ImGui::BeginDisabled();
    if (ui::IconButton(icon, label)) Later([type, rows = AddTargets()] { DoAdd(type, {}, false, &rows); });
    if (ok == 0) ImGui::EndDisabled();
    ui::ItemTipText(why.c_str());
}

// 13-7a (audit B17 / C3.2): the "?" circle at the toolbar's right end; a click opens the mockup's
// help (each type's icon, its name and what it does); the shortcuts on hover.
void ToolbarHelp()
{
    if (ui::HelpMark("##tbhelpmark", "What do Switch, Random, Blend and Sound do?\n\n"
                                     "Shortcuts: F2 rename, Delete remove, Ctrl+D duplicate, Ctrl+X / Ctrl+V move,\n"
                                     "Ctrl+Z undo, Space play. Ctrl / Shift-click selects several rows."))
        ImGui::OpenPopup("##tbhelp");
    if (!ui::BeginMenuPopup("##tbhelp")) return;
    struct Line {
        SoundNodeType type;
        const char*   name;
        const char*   text;
    };
    static const Line kLines[4] = {
        {SoundNodeType::Switch, "Switch", "plays one child, chosen by a Switch Group value (Material = Grass...)."},
        {SoundNodeType::Random, "Random", "plays one of its children, picked at random at each event."},
        {SoundNodeType::Blend, "Blend", "plays all its children together, layered."},
        {SoundNodeType::Sound, "Sound", "one file or item (its Takes / take markers are its variations)."},
    };
    const float wrap = ui::Dp(336.0f);
    for (const Line& l : kLines) {
        ImDrawList*  dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  is = ui::Dp(14.0f), lh = ImGui::GetTextLineHeight();
        DrawNodeTypeGlyph(dl, ImVec2(p.x, p.y + std::floor((lh - is) * 0.5f)), is, l.type, TypeColor(l.type));
        ImGui::SetCursorScreenPos(ImVec2(p.x + is + ui::Dp(6.0f), p.y));
        {
            ui::FontScope f(ui::FontSemibold());
            ImGui::TextUnformatted(l.name);
        }
        ImGui::SameLine(0.0f, ui::Dp(5.0f));
        ui::ToneText(l.text, ui::kMuted, true, std::max(ui::Dp(80.0f), wrap - (ImGui::GetCursorScreenPos().x - p.x)));
    }
    ui::EndMenuPopup();
}

void Toolbar(const SoundLibrary& lib)
{
    // The mockup's .tb well of .tbb buttons (24 px, pad 0 6, 3 px apart, wrapping inside it); a
    // disabled one takes .tbb.dis (transparent, faint label and icon).
    ui::Well well;
    ui::BeginToolbar(well);
    [&] {
        ToolbarAdd(lib, SoundNodeType::Sound, "+ Sound");
        if (g.changed) return;
        SameLineIfFits(ui::IconButtonWidth("+ Random"));
        ToolbarAdd(lib, SoundNodeType::Random, "+ Random");
        if (g.changed) return;
        SameLineIfFits(ui::IconButtonWidth("+ Blend"));
        ToolbarAdd(lib, SoundNodeType::Blend, "+ Blend");
        if (g.changed) return;
        SameLineIfFits(ui::IconMenuButtonWidth("+ Switch"));
        ToolbarAdd(lib, SoundNodeType::Switch, "+ Switch");
        if (g.changed) return;
        if (g.sel.size() > 1) {
            // The mockup's .tbto chip: "→ N".
            const std::string cnt = "\xE2\x86\x92 " + std::to_string(g.sel.size());
            SameLineIfFits(ui::CountChipWidth(cnt.c_str()));
            ui::CountChip(cnt.c_str());
            ui::ItemTipText("The + buttons add one to each selected row");
        }
        int with_node = 0;
        for (const LibraryRow& r : g.sel)
            if (!RowNode(lib, r).empty()) ++with_node;
        const std::string wl = with_node > 1 ? "Group (" + std::to_string(with_node) + ")" : std::string("Wrap");
        // The .tbsep before Wrap (a 1 x 18 stroke line, 2 px each side), only beside the buttons:
        // when Wrap goes onto a line of its own, no separator leads it.
        const float sep_w = ui::Dp(5.0f);
        if (SameLineIfFits(sep_w + ImGui::GetStyle().ItemSpacing.x + MenuButtonWidth(wl.c_str()))) {
            const ImVec2 sp = ImGui::GetCursorScreenPos();
            const float  fh = ImGui::GetFrameHeight();
            ImGui::Dummy(ImVec2(sep_w, fh));
            const float x = std::floor(sp.x + sep_w * 0.5f) + 0.5f, half = ui::Dp(9.0f);
            ImGui::GetWindowDrawList()->AddLine(ImVec2(x, sp.y + fh * 0.5f - half), ImVec2(x, sp.y + fh * 0.5f + half),
                                                ui::kStroke, 1.0f);
            ImGui::SameLine();
        }
        if (MenuButton(wl.c_str(), with_node > 0)) ImGui::OpenPopup("##tbwrap");
        ui::ItemTipText(with_node > 1 ? "Groups the selected rows into a new Random or Blend"
                                      : with_node ? "Wraps the selected row in a new Random or Blend"
                                                  : "Select rows first (Ctrl / Shift-click for several)");
        if (ui::BeginMenuPopup("##tbwrap")) {
            std::vector<LibraryRow> rows;
            for (const LibraryRow& r : g.sel)
                if (!RowNode(lib, r).empty()) rows.push_back(r);
            WrapMenu(rows);
            ui::EndMenuPopup();
        }
        if (g.changed) return;
        // The "?" at the far right of the last line (a flex spacer before it in the mockup).
        const float qs = ui::Dp(18.0f);
        if (SameLineIfFits(qs)) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - qs));
        ToolbarHelp();
    }();
    ui::EndToolbar(well);
}

// ---- Rows ---------------------------------------------------------------------------------------

void ClickRow(const TreeRow& r)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl) {
        auto it = std::find(g.sel.begin(), g.sel.end(), r.row);
        if (it != g.sel.end()) g.sel.erase(it);
        else g.sel.push_back(r.row);
        g.anchor = r.row;
        g.has_anchor = true;
    } else if (io.KeyShift && g.has_anchor) {
        g.sel = RangeSelectByLevel(g.rows, g.anchor, r.row);
        if (g.sel.empty()) SelectOnly(r.row);
    } else {
        SelectOnly(r.row);
    }
}

void DrawRowBody(const SoundLibrary& lib, const TreeRow& r);

void DrawRow(const SoundLibrary& lib, const TreeRow& r, size_t index)
{
    ImGui::PushID(static_cast<int>(index));
    ImGui::PushID(r.row.key.c_str());
    const float y0 = ImGui::GetCursorScreenPos().y;
    DrawRowBody(lib, r);
    if (g.has_scroll_to && r.row == g.scroll_to && !g.changed) {  // 13-17d: the arrow keys' row in view
        g.has_scroll_to = false;
        const float  y1 = std::max(y0 + 1.0f, ImGui::GetCursorScreenPos().y);
        const ImVec2 wp = ImGui::GetWindowPos();
        const float  wh = ImGui::GetWindowHeight();
        if (y0 < wp.y) ImGui::SetScrollFromPosY(y0 - wp.y, 0.0f);
        else if (y1 > wp.y + wh) ImGui::SetScrollFromPosY(y1 - wp.y, 1.0f);
    }
    ImGui::PopID();
    ImGui::PopID();
}

// `c` at .45 opacity (the mockup's cut rows, .trow.cutting).
ImU32 Faded(ImU32 c, bool fade)
{
    if (!fade) return c;
    const unsigned a = (c >> IM_COL32_A_SHIFT) & 0xFFu;
    return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a * 45u / 100u) << IM_COL32_A_SHIFT);
}

// 13-7a (audit C3.5): the Unassigned header (.trow.uhead): 24 px after a 4 px margin, its caption
// UPPERCASE letter-spaced in warn 11 px, then its hint in faint.
void DrawUnassignedHeader(const TreeRow& r)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float  top = ui::Dp(4.0f), h = ui::Dp(24.0f);
    const float  x = p.x + ui::Dp(kRowPad + r.depth * kIndent) + ui::Dp(18.0f + 6.0f);
    std::string  caps = r.label, rest;
    const size_t dot = caps.find(" \xC2\xB7 ");
    if (dot != std::string::npos) {
        rest = caps.substr(dot);
        caps.resize(dot);
    }
    ImFont*     f = ui::FontSmall();
    const float ty = std::floor(p.y + top + (h - f->FontSize) * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(x, ty));
    ui::GroupCaption(caps.c_str(), kLibUnassigned);
    const float cx = ImGui::GetItemRectMax().x;
    if (!rest.empty()) {
        ui::FontScope fs(f);
        ui::EllipsisTextTip(ImGui::GetWindowDrawList(), ImVec2(cx, ty), p.x + w - ui::Dp(4.0f), ui::kFaint, rest);
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, top + h));
}

// One row; returns at once after a gesture (`lib` is then stale).
// 13-7a (audit C3.3 - C3.5, B10): the mockup's .trow, 28 px: from the left 4 px + 18 px a level,
// the 18 px chevron (or its empty room), the type icon (a container's a 20 px button in the
// accent ink, framed while hovered or selected; a Sound's 16 px muted; an empty value's dot),
// the name in 13 px; from the right, each shown only while the row is hovered / selected and
// taking no room otherwise: the ⋯ menu, ▶ / ■, the "default" tag or ☆, then "missing".
void DrawRowBody(const SoundLibrary& lib, const TreeRow& r)
{
    if (r.kind == TreeRowKind::UnassignedHeader) {
        DrawUnassignedHeader(r);
        return;
    }
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  rh = ui::RowH();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float  x0 = p.x + ui::Dp(kRowPad + r.depth * kIndent);
    const float  cy = p.y + rh * 0.5f;
    const float  gap = ui::Dp(6.0f);

    if (r.kind == TreeRowKind::Hint) {
        // .trow.hintrow: a dashed box at the row's depth, its text faint 12 px.
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##row", ImVec2(w, rh));
        const ImVec2 a(p.x + ui::Dp(r.depth * kIndent), p.y + ui::Dp(2.0f)), b(p.x + w, p.y + rh - ui::Dp(2.0f));
        RowDropTarget(lib, r, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (g.changed) return;
        const bool drag = ImGui::GetDragDropPayload() != nullptr;
        ui::DashedRect(dl, a, b, drag ? ui::kAccentLine : ui::kStrokeStrong, ui::kRadiusSm);
        ui::EllipsisTextTip(dl, ImVec2(a.x + ui::Dp(4.0f) + ui::Dp(18.0f) + gap, cy - ImGui::GetTextLineHeight() * 0.5f),
                            b.x - ui::Dp(4.0f), drag ? ui::kMuted : ui::kFaint, r.label);
        return;
    }

    const bool       selected = IsSel(r.row);
    const bool       is_cut = g.clip_lib == lib.uid && std::find(g.clip.begin(), g.clip.end(), r.row) != g.clip.end();
    const SoundNode* n = r.node.empty() ? nullptr : FindNode(lib, r.node);
    const bool       lit = g.hooks.is_lit && n && g.hooks.is_lit(n->uid);
    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ui::SelectRow("##row", selected, lit ? ui::kRowLit : 0u, rh,
                                       ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap);
    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    // Hover of the whole row (its overlaid buttons included), so they do not flicker.
    const bool hovered = ui::SelectRowHovered();
    if (r.unassigned && !selected && !lit) dl->AddRectFilled(r0, r1, ui::kUnassignedSoft, ui::kRadiusSm);
    if (clicked) ClickRow(r);
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && (!r.node.empty() || r.kind == TreeRowKind::Value))
        StartRowRename(lib, r.row);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        if (!selected) SelectOnly(r.row);
        g.menu_row = r.row;
        g.menu_rows = IsSel(r.row) ? g.sel : std::vector<LibraryRow>{r.row};
        g.open_row_menu = true;
    }
    RowDragSource(lib, r);
    RowDropTarget(lib, r, r0, r1);
    if (g.changed) return;

    // Chevron (an 18 px button) or its empty room.
    const float cs = ui::Dp(18.0f);
    if (r.container) {
        ImGui::SetCursorScreenPos(ImVec2(x0, std::floor(cy - cs * 0.5f)));
        if (ui::Chevron("##chev", r.open, cs)) {
            if (r.open) g.closed.insert(r.node);
            else g.closed.erase(r.node);
        }
    }
    // Type icon (a container's is a button: its type menu).
    const float ix = x0 + cs + gap;
    float       icon_w = ui::Dp(16.0f);
    if (n && r.container) {
        const float  box = ui::Dp(20.0f);
        const ImVec2 ip(ix, std::floor(cy - box * 0.5f));
        ImGui::SetCursorScreenPos(ip);
        if (ImGui::InvisibleButton("##type", ImVec2(box, box))) ImGui::OpenPopup("##typemenu");
        const bool hot = ImGui::IsItemHovered();
        if (hot) {
            std::string tip = TypeWord(n->type);
            if (n->type == SoundNodeType::Switch)
                if (const SwitchGroup* gr = FindGroup(GetSoundStore(nullptr), n->group_uid)) tip += " on " + gr->name;
            ui::ItemTip("%s \xC2\xB7 click to change the type", tip.c_str());
        }
        DrawTypeIcon(dl, ip, box, n->type, hot || selected || ImGui::IsPopupOpen("##typemenu"), Faded(TypeColor(n->type), is_cut));
        icon_w = box;
    } else if (n) {
        const float is = ui::Dp(16.0f);
        DrawTypeIcon(dl, ImVec2(ix, std::floor(cy - is * 0.5f)), is, n->type, false, Faded(TypeColor(n->type), is_cut));
    } else {
        // A value with no sound of its own: the mockup's value dot (ICON.value).
        const float is = ui::Dp(16.0f);
        DrawLibraryGlyph(dl, ImVec2(ix, std::floor(cy - is * 0.5f)), is, kGlyphValue, Faded(ui::kMuted, is_cut));
    }
    if (ui::BeginMenuPopup("##typemenu")) {
        TypeMenu(IsSel(r.row) ? g.sel : std::vector<LibraryRow>{r.row});
        ui::EndMenuPopup();
    }
    if (g.changed) return;

    // Right side, from the right edge.
    float      rx = p.x + w - ui::Dp(4.0f);
    const bool more_on = (g.open_row_menu || g.row_menu_open) && g.menu_row == r.row;
    const bool show_tools = hovered || selected || more_on || ImGui::IsPopupOpen("##typemenu");
    if (show_tools) {
        const float bw = ui::Dp(22.0f), bh = ui::Dp(20.0f);
        rx -= bw;
        bool hov = false;
        if (RowToolButton("##more", ImVec2(rx, std::floor(cy - bh * 0.5f)), bw, bh, more_on, &hov)) {
            if (!selected) SelectOnly(r.row);
            g.menu_row = r.row;
            g.menu_rows = IsSel(r.row) ? g.sel : std::vector<LibraryRow>{r.row};
            g.open_row_menu = true;
        }
        CentredGlyph(dl, ui::FontTitle(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), hov ? ui::kText : ui::kMuted,
                     "\xE2\x8B\xAF");
        rx -= gap;
    }
    if ((show_tools || lit) && n && g.hooks.play) {
        const float bs = ui::Dp(20.0f);
        rx -= bs;
        bool hov = false;
        if (RowToolButton("##play", ImVec2(rx, std::floor(cy - bs * 0.5f)), bs, bs, lit, &hov)) {
            if (lit && g.hooks.stop) g.hooks.stop();
            else g.hooks.play(lib.uid, n->uid);
        }
        ui::ItemTipText(lit ? "Stop" : "Play");
        const ImU32 pc = hov || lit ? ui::kAccent : ui::kMuted;
        const float c = rx + bs * 0.5f, s = ui::Dp(4.0f);
        if (lit) dl->AddRectFilled(ImVec2(c - s, cy - s), ImVec2(c + s, cy + s), pc);
        else dl->AddTriangleFilled(ImVec2(c - s, cy - s - 1.0f), ImVec2(c - s, cy + s + 1.0f), ImVec2(c + s + 1.0f, cy), pc);
        rx -= gap;
    }
    if (r.kind == TreeRowKind::Value) {
        if (r.is_default) {
            // 13-7 (audit B3): the mockup's .dtag box (10 px muted, stroke-strong edge).
            const char* t = "default";
            const float tw = ui::SmallTagWidth(t);
            rx -= tw;
            ui::DrawSmallTag(dl, ImVec2(rx, std::floor(cy - ui::SmallTagHeight() * 0.5f)), t);
            ImGui::SetCursorScreenPos(ImVec2(rx, p.y));
            ImGui::Dummy(ImVec2(tw, rh));
            ui::ItemTipText("Default value: plays when the event's value is not set, or when a value has no sound of its own");
            rx -= gap;
        } else if (hovered) {
            // .star: 18 px, faint, warn on its hover background.
            const float bs = ui::Dp(18.0f);
            rx -= bs;
            bool hov = false;
            if (RowToolButton("##star", ImVec2(rx, std::floor(cy - bs * 0.5f)), bs, bs, false, &hov))
                Later([row = r.row] { DoMakeDefault({row}); });
            CentredGlyph(dl, ui::FontSmall(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), hov ? ui::kWarn : ui::kFaint,
                         "\xE2\x98\x86");
            ui::ItemTipText("Make it the default value");
            rx -= gap;
        }
    }
    if (g.changed) return;
    // 13-21: a library gap on this node, or hidden under it (folded, searched out): an amber "!",
    // the reasons on hover.
    if (n && r.kind != TreeRowKind::UnassignedHeader) {
        std::string tip;
        const auto  own = g.gaps.find(n->uid);
        if (own != g.gaps.end()) tip = own->second + "\nNothing plays from it.";
        if (r.container) {
            std::string inside;
            for (const LibraryGap& h : g.hidden_gaps)
                if (h.node != n->uid && IsInSubtree(lib, h.node, n->uid)) inside += "\n" + h.reason;
            if (!inside.empty()) tip += (tip.empty() ? "" : "\n") + std::string("Gaps inside:") + inside;
        }
        if (!tip.empty()) {
            ImFont*     f = ui::FontSemibold();
            const float tw = ui::TextSizeIn(f, "!").x + ui::Dp(2.0f);
            rx -= tw;
            ui::AddTextIn(dl, f, ImVec2(rx + ui::Dp(1.0f), std::floor(cy - f->FontSize * 0.5f)), Faded(ui::kWarn, is_cut), "!");
            ImGui::SetCursorScreenPos(ImVec2(rx, p.y));
            ImGui::Dummy(ImVec2(tw, rh));
            ui::ItemTipText(tip.c_str());
            rx -= gap;
        }
    }
    const bool missing = n && SoundMissing(*n);
    if (missing) {
        // .rt.miss: 12 px #ff9a9a.
        const char* t = "missing";
        rx -= ImGui::CalcTextSize(t).x;
        dl->AddText(ImVec2(rx, cy - ImGui::GetTextLineHeight() * 0.5f), Faded(ui::kMissText, is_cut), t);
        rx -= gap;
    } else if (lib.user && n && n->type == SoundNodeType::Sound && n->sound.source.kind == SoundSourceKind::Item &&
               r.kind == TreeRowKind::Node) {
        // 13-24 (C6): an item Sound in a User Library plays only in this project.
        const char* t = "project";
        const float tw = ui::SmallTagWidth(t);
        rx -= tw;
        ui::DrawSmallTag(dl, ImVec2(rx, std::floor(cy - ui::SmallTagHeight() * 0.5f)), t);
        ImGui::SetCursorScreenPos(ImVec2(rx, p.y));
        ImGui::Dummy(ImVec2(tw, rh));
        ui::ItemTipText("Project only: an item of this project, missing in other projects");
        rx -= gap;
    }

    // The name (or its inline rename field), 13 px.
    const float nx = ix + icon_w + gap;
    if (g.rename == RenameKind::Row && g.r_a == lib.uid && g.r_row == r.row) {
        ImGui::SetCursorScreenPos(ImVec2(nx, std::floor(cy - ui::ControlH() * 0.5f)));
        const int res = InlineName(rx - nx);
        if (res != 0) {
            const std::string name = g.buf;
            const LibraryRow row = r.row;
            const std::string lib_uid = lib.uid;
            EndRename();
            if (res > 0) Later([lib_uid, row, name] { Gesture("Rename", [&](SoundStore& s) { return RenameRow(s, lib_uid, row, name); }); });
        }
    } else {
        ImU32 col = ui::kText;
        if (r.kind == TreeRowKind::Value && r.node.empty()) col = ui::kMuted;  // an empty value (.nm.emptyv)
        if (r.unassigned) col = ui::kUnassignedName;
        if (missing && r.kind == TreeRowKind::Node) col = ui::kMissName;
        ui::FontScope f(ui::FontTitle());
        ui::EllipsisTextTip(dl, ImVec2(nx, std::floor(cy - ImGui::GetTextLineHeight() * 0.5f)), rx, Faded(col, is_cut), r.label);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rh));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

// 13-17d (Antho 2026-10-13): the arrow keys walk the tree's rows (last frame's g.rows). Up / Down:
// the row above / below (none selected: the first). Right opens the selected container, Left
// folds it; on anything else they do nothing (Antho: Left / Right only open and close).
void ArrowKeys()
{
    const bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow), down = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
    const bool left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow), right = ImGui::IsKeyPressed(ImGuiKey_RightArrow);
    if (!up && !down && !left && !right) return;
    auto pickable = [](const TreeRow& r) { return r.kind == TreeRowKind::Node || r.kind == TreeRowKind::Value; };
    int cur = -1;
    if (!g.sel.empty())
        for (size_t i = 0; i < g.rows.size(); ++i)
            if (g.rows[i].row == g.sel.back()) cur = static_cast<int>(i);
    const int n = static_cast<int>(g.rows.size());
    auto go = [&](int i) {
        if (i < 0 || i >= n) return;
        SelectOnly(g.rows[static_cast<size_t>(i)].row);
        g.scroll_to = g.rows[static_cast<size_t>(i)].row;
        g.has_scroll_to = true;
    };
    if (cur < 0) {
        for (int i = 0; i < n; ++i)
            if (pickable(g.rows[static_cast<size_t>(i)])) return go(i);
        return;
    }
    const TreeRow& r = g.rows[static_cast<size_t>(cur)];
    if (up || down) {
        const int step = down ? 1 : -1;
        for (int i = cur + step; i >= 0 && i < n; i += step)
            if (pickable(g.rows[static_cast<size_t>(i)])) return go(i);
    } else if (r.container && right && !r.open) {
        g.closed.erase(r.node);
    } else if (r.container && left && r.open) {
        g.closed.insert(r.node);
    }
}

// The keys of decision 7, while the Library view has the focus and no field takes them.
void TreeKeys(const SoundLibrary& lib)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (!LibraryViewFocused() || !g.tree_focused || io.WantTextInput || g.rename != RenameKind::None) return;
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
    const LibraryRow* first = g.sel.empty() ? nullptr : &g.sel.front();
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !g.sel.empty()) Later([rows = g.sel] { DoRemove(rows); });
    else if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && first) StartRowRename(lib, *first);
    else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && !g.sel.empty()) Later([rows = g.sel] { DoDuplicate(rows); });
    else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X, false) && !g.sel.empty()) {
        DoCut(lib.uid, g.sel);
    } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        Later([lib_uid = lib.uid, has = first != nullptr, row = first ? *first : LibraryRow{}] {
            DoPaste(lib_uid, has ? &row : nullptr);
        });
    } else if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false) && first) {
        const bool lit = g.hooks.is_lit && g.hooks.is_lit(RowNode(lib, *first));
        if (lit && g.hooks.stop) g.hooks.stop();
        else PlayRow(lib, *first);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (g.hooks.stop) g.hooks.stop();
        g.clip.clear();
    } else if (!io.KeyCtrl && !io.KeyShift) {
        ArrowKeys();
    }
}

// A line of text centred in [x, x + w] (wrapped within it, from its left, when it does not fit).
void CentredText(const char* text, ImU32 col, float x, float w)
{
    const float tw = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorScreenPos(ImVec2(tw <= w ? std::floor(x + (w - tw) * 0.5f) : x, ImGui::GetCursorScreenPos().y));
    ui::ToneText(text, col, true, std::max(1.0f, tw <= w ? tw + 1.0f : w));
}

// 13-7a (audit C3.6): the mockup's .empty-big -- a dashed box (stroke-strong, r7, 18 px above and
// below, 8 px each side, 18 px inside), centred muted text, the four add buttons with their icons,
// then the drop sentence; the box takes rows and Media Explorer files (accent edge + accent-soft
// while an acceptable drag is over it).
void EmptyLibraryHint(const SoundLibrary& lib)
{
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    const float  avail = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float  mx = std::min(ui::Dp(8.0f), avail * 0.1f), my = ui::Dp(18.0f);
    const float  pad = std::min(ui::Dp(18.0f), std::max(ui::Dp(4.0f), (avail - 2.0f * mx) * 0.1f));
    const ImVec2 b0(c0.x + mx, c0.y + my);
    const float  bw = std::max(1.0f, avail - 2.0f * mx);
    const float  ix = b0.x + pad, iw = std::max(1.0f, bw - 2.0f * pad);
    const float  vgap = ui::Dp(10.0f);

    ImGuiWindow* win = ImGui::GetCurrentWindow();
    const float  saved_work = win->WorkRect.Max.x, saved_content = win->ContentRegionRect.Max.x;
    win->WorkRect.Max.x = std::min(saved_work, ix + iw);
    win->ContentRegionRect.Max.x = std::min(saved_content, ix + iw);
    ImGui::SetCursorScreenPos(ImVec2(ix, b0.y + pad));
    ImGui::BeginGroup();
    CentredText("This library is empty.", ui::kMuted, ix, iw);
    CentredText("Add its first sound, or a container:", ui::kMuted, ix, iw);
    ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, vgap - ImGui::GetStyle().ItemSpacing.y)));
    const char* const   kLabels[4] = {"+ Sound##hint", "+ Random##hint", "+ Blend##hint", "+ Switch##hint"};
    const SoundNodeType kHintTypes[4] = {SoundNodeType::Sound, SoundNodeType::Random, SoundNodeType::Blend,
                                         SoundNodeType::Switch};
    float row_w = 3.0f * ImGui::GetStyle().ItemSpacing.x;
    for (const char* l : kLabels) row_w += ui::IconButtonWidth(l);
    ImGui::SetCursorScreenPos(ImVec2(row_w <= iw ? std::floor(ix + (iw - row_w) * 0.5f) : ix, ImGui::GetCursorScreenPos().y));
    for (int i = 0; i < 4 && !g.changed; ++i) {
        if (i > 0) SameLineIfFits(ui::IconButtonWidth(kLabels[i]));
        if (!ui::IconButton(TypeIcon(kHintTypes[i]), kLabels[i])) continue;
        if (kHintTypes[i] == SoundNodeType::Switch) ImGui::OpenPopup("##hintswitch");
        else Later([type = kHintTypes[i], rows = AddTargets()] { DoAdd(type, {}, false, &rows); });
    }
    if (!g.changed && ui::BeginMenuPopup("##hintswitch")) {
        ui::MenuCaption("Switch on");
        bool ng = false;
        const std::string gid = GroupMenuItems(true, &ng);
        if (!gid.empty() || ng) Later([gid, ng, rows = AddTargets()] { DoAdd(SoundNodeType::Switch, gid, ng, &rows); });
        ui::EndMenuPopup();
    }
    ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, vgap - ImGui::GetStyle().ItemSpacing.y)));
    CentredText("or drop files from the Media Explorer here.", ui::kMuted, ix, iw);
    ImGui::EndGroup();
    win->WorkRect.Max.x = saved_work;
    win->ContentRegionRect.Max.x = saved_content;
    const float  bottom = ImGui::GetItemRectMax().y + pad;
    const ImVec2 b1(b0.x + bw, bottom);
    if (g.changed) return;  // `lib` is stale
    ui::DashedRect(ImGui::GetWindowDrawList(), b0, b1, ui::kStrokeStrong, ui::kRadiusMd);
    // The box is a drop target: the root.
    ImGui::SetCursorScreenPos(b0);
    ImGui::Dummy(ImVec2(bw, bottom - b0.y));
    if (ImGui::BeginDragDropTarget()) {
        RootDropOn(lib, b0, b1);
        ImGui::EndDragDropTarget();
    }
    ImGui::SetCursorScreenPos(ImVec2(c0.x, bottom + my));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

}  // namespace

// ---- Column 1 -----------------------------------------------------------------------------------

namespace {

void StartLibraryRename(const SoundLibrary& lib)
{
    g.rename = RenameKind::Library;
    g.r_a = lib.uid;
    std::snprintf(g.buf, sizeof(g.buf), "%s", lib.name.c_str());
    g.r_focus = true;
}

void PickLibrary(const std::string& uid)
{
    if (g.lib == uid) return;
    g.lib = uid;
    g.sel.clear();
    g.has_anchor = false;
}

// A list caption (.lgrp: 11 px faint UPPERCASE, letter-spaced, margin 6 2 2).
void ListCaption(const char* text, const char* tip = nullptr)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(p.x + ui::Dp(2.0f), p.y + ui::Dp(6.0f) - ImGui::GetStyle().ItemSpacing.y));
    ui::GroupCaption(text);
    if (tip) ui::ItemTipText(tip);
    ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, ui::Dp(2.0f) - ImGui::GetStyle().ItemSpacing.y)));
}

// The library's menu (the row's right-click, the tree header's ⋯): Rename, a User Library's
// Export / Locate / Refresh, Delete. Sets *to_delete for the caller to delete after its loop.
void LibraryMenuItems(const SoundLibrary& lib, std::string* to_delete, bool rename_item)
{
    if (rename_item && ImGui::MenuItem("Rename")) StartLibraryRename(lib);
    // 13-38: offered in every library's menu (Project Libraries included); it looks for the missing
    // File Sounds of every library, so it is enabled when any library has one.
    std::string first;
    const int   miss = UserLibraryMissing(lib.uid, &first);
    int         miss_all = 0;
    for (const SoundLibrary& l : GetSoundStore(nullptr).libraries) miss_all += UserLibraryMissing(l.uid, nullptr);
    if (ImGui::MenuItem("Locate missing files in a folder...", nullptr, false, miss_all > 0))
        RequestLibraryLocateFolder();
    ui::ItemTipText(miss_all > 0 ? "Pick the folder that holds the sounds (for example the soundbank's parent): RAV "
                                   "searches it and its subfolders for the missing sounds of every library"
                                 : "No missing sound in any library");
    if (lib.user) {
        if (ImGui::MenuItem("Export .ravlib...")) RequestUserLibraryExport(lib.uid);
        if (ImGui::MenuItem("Locate a missing file...", nullptr, false, miss > 0)) RequestUserLibraryLocate(lib.uid, first);
        ui::ItemTipText(miss > 0 ? "Pick the file of its first missing sound: the others are looked for near it"
                                 : "No missing sound");
        if (ImGui::MenuItem("Refresh the User Library folder")) RefreshUserLibraries();
    } else {
        // 13-24: reusable in other projects; its item Sounds become File Sounds of their files.
        if (ImGui::MenuItem("Convert to User Library")) {
            const std::string uid = lib.uid;
            Later([uid] { ConvertToUserLibrary(uid); });
        }
        ui::ItemTipText("Save it as a User Library (.ravlib) for other projects. Sounds whose item plays a media file "
                        "use that file; the others (MIDI, piles, ...) stay project-only");
    }
    // 13-15c: the body parts this library suggests itself for (its kit, stored in the library): a tick per part,
    // each toggle one undo point.
    if (ui::BeginSubMenu("Kit...")) {
        ui::MenuCaption("SERVES THESE BODY PARTS");
        for (const BodyPart& bp : BodyParts()) {
            if (bp.id == "other") continue;
            const bool on = KitServes(lib, bp.id);
            if (ImGui::MenuItem(bp.label.c_str(), nullptr, on)) {
                std::vector<std::string> parts = lib.kit;
                if (on) parts.erase(std::remove(parts.begin(), parts.end(), bp.id), parts.end());
                else parts.push_back(bp.id);
                const std::string uid = lib.uid;
                Later([uid, parts] {
                    Gesture("Set library kit", [&](SoundStore& st) {
                        SoundLibrary* l = FindLibrary(st, uid);
                        if (!l || !SetKitParts(*l, parts)) return false;
                        l->kit_offered = true;  // set by hand here: no "use it?" offer later
                        return true;
                    });
                });
            }
        }
        ui::ItemTipText("A kit is a suggestion: Assign sounds > Project can use it as the project default");
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Delete library")) *to_delete = lib.uid;
}

// 13-7a (audit C1.3): a library row (.lrow): 28 px, r7, the 8 px dot in the library's colour,
// the name in 13 px, then the blocked "!" (12 px bold warn) and the missing count (11 px semibold
// bad). The trail stays beside the name while the name keeps ~4 em, else it goes under it.
void LibraryListRow(const SoundStore& store, const SoundLibrary& lib, std::string* to_delete)
{
    ImGui::PushID(lib.uid.c_str());
    const float  rh = ui::RowH();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  nx = p.x + ui::Dp(6.0f + 8.0f + 7.0f);
    if (g.rename == RenameKind::Library && g.r_a == lib.uid) {
        ImGui::SetCursorScreenPos(ImVec2(nx, std::floor(p.y + (rh - ui::ControlH()) * 0.5f)));
        const int res = InlineName(p.x + w - nx);
        if (res != 0) {
            const std::string name = g.buf, uid = lib.uid;
            EndRename();
            if (res > 0) Later([uid, name] { Gesture("Rename library", [&](SoundStore& s) { return RenameLibrary(s, uid, name); }); });
        }
        dl->AddCircleFilled(ImVec2(p.x + ui::Dp(10.0f), p.y + rh * 0.5f), ui::Dp(4.0f), LibDot(lib.color));
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rh));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::PopID();
        return;
    }
    const LibraryCheck chk = CheckLibrary(store, lib);
    const int          miss = CountMissing(lib, SoundMissing);
    const std::string  miss_t = miss > 0 ? std::to_string(miss) : std::string();
    ImFont*            blk_f = ui::FontSemibold();
    ImFont*            miss_f = ui::FontCaptionBold();
    float              tw[2];
    int                tk[2];
    int                nt = 0;
    // 13-21: a red "!" when blocked, an amber one for gaps; the tooltip lists every reason.
    std::string chk_tip;
    if (chk.blocked) chk_tip = "Blocked: " + chk.reason;
    if (!chk.warnings.empty()) {
        if (!chk_tip.empty()) chk_tip += "\n";
        chk_tip += "Gaps (events on them place nothing):";
        for (const LibraryGap& gp : chk.warnings) chk_tip += "\n- " + gp.reason;
    }
    if (!chk_tip.empty()) { tk[nt] = 0; tw[nt++] = ui::TextSizeIn(blk_f, "!").x + ui::Dp(2.0f); }
    if (miss > 0) { tk[nt] = 1; tw[nt++] = ui::TextSizeIn(miss_f, miss_t.c_str()).x; }
    flow::TrailPlace tp[2];
    float            name_w = 0.0f;
    {
        ui::FontScope f(ui::FontTitle());
        name_w = ui::TextWidth(lib.name.c_str());
    }
    const float lead_min = std::min(name_w, 4.0f * ImGui::GetFontSize());
    const int   lines = nt > 0 ? flow::PlaceTrail(tw, nt, nx, p.x + w - ui::Dp(6.0f), lead_min, ui::Dp(7.0f), tp) : 1;
    ImGui::SetNextItemAllowOverlap();
    if (ui::SelectRow("##lib", g.lib == lib.uid, 0, rh * static_cast<float>(lines),
                      ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap, ui::kRadiusMd))
        PickLibrary(lib.uid);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) StartLibraryRename(lib);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##libmenu");
    dl->AddCircleFilled(ImVec2(p.x + ui::Dp(10.0f), p.y + rh * 0.5f), ui::Dp(4.0f), LibDot(lib.color));
    for (int i = 0; i < nt; ++i) {
        const float y = p.y + static_cast<float>(tp[i].line) * rh;
        ImFont*     f = tk[i] == 0 ? blk_f : miss_f;
        const char* t = tk[i] == 0 ? "!" : miss_t.c_str();
        ImGui::SetCursorScreenPos(ImVec2(tp[i].x, y));
        ImGui::Dummy(ImVec2(tp[i].w, rh));
        ui::AddTextIn(dl, f, ImVec2(tp[i].x + (tk[i] == 0 ? ui::Dp(1.0f) : 0.0f), std::floor(y + (rh - f->FontSize) * 0.5f)),
                      tk[i] == 0 && !chk.blocked ? ui::kWarn : ui::kBad, t);
        if (tk[i] == 0) ui::ItemTipText(chk_tip.c_str());
        else ui::ItemTip("%d missing sound(s)", miss);
    }
    {
        ui::FontScope f(ui::FontTitle());
        ui::EllipsisTextTip(dl, ImVec2(nx, std::floor(p.y + (rh - ImGui::GetTextLineHeight()) * 0.5f)),
                            (lines == 1 && nt > 0 ? tp[0].x - ui::Dp(7.0f) : p.x + w - ui::Dp(6.0f)), ui::kText, lib.name);
    }
    if (ui::BeginMenuPopup("##libmenu")) {
        LibraryMenuItems(lib, to_delete, true);
        ui::EndMenuPopup();
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rh * static_cast<float>(lines)));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopID();
}

// 13-7a (audit C1.4): a .ravlib of the User Library folder not in this project, under USER:
// dimmed, its dot, an "Add" button (or "unreadable").
void UserFileRow(const UserLibraryFile& f, std::string* add_file)
{
    ImGui::PushID(f.file.c_str());
    const float  rh = ui::RowH();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  nx = p.x + ui::Dp(6.0f + 8.0f + 7.0f);
    const char*  trail = f.readable ? "Add" : "unreadable";
    const float  tw = f.readable ? ui::ButtonWidth(trail) : ui::TextWidth(trail);
    ImGui::SetNextItemAllowOverlap();
    ui::SelectRow("##ufile", false, 0, rh, ImGuiSelectableFlags_AllowOverlap, ui::kRadiusMd);
    if (f.readable) ui::ItemTip("%s\nNot in this project: Add puts it in", f.file.c_str());
    else ui::ItemTip("%s\nThis file cannot be read", f.file.c_str());
    if (f.readable) dl->AddCircleFilled(ImVec2(p.x + ui::Dp(10.0f), p.y + rh * 0.5f), ui::Dp(4.0f), Faded(LibDot(f.color), true));
    const float tx = std::max(nx, p.x + w - ui::Dp(6.0f) - tw);
    if (f.readable) {
        ImGui::SetCursorScreenPos(ImVec2(tx, std::floor(p.y + (rh - ui::ControlH()) * 0.5f)));
        if (ui::SolidButton(trail, ImVec2(std::min(tw, p.x + w - ui::Dp(6.0f) - tx), 0.0f))) *add_file = f.file;
    } else {
        dl->AddText(ImVec2(tx, std::floor(p.y + (rh - ImGui::GetTextLineHeight()) * 0.5f)), ui::kFaint, trail);
    }
    {
        ui::FontScope fs(ui::FontTitle());
        ui::EllipsisTextTip(dl, ImVec2(nx, std::floor(p.y + (rh - ImGui::GetTextLineHeight()) * 0.5f)), tx - ui::Dp(7.0f),
                            f.readable ? ui::kMuted : ui::kFaint, f.name, f.file.c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rh));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopID();
}

// 13-7a (audit C1.1 - C1.4): a fixed "Libraries" section, « at its header's right; the project's
// libraries under PROJECT, the User Library's under USER (with the folder's files not in the
// project); "+ Library" and "+ User Library" side by side, then "Import .ravlib..." full width.
void LibrariesSectionWidgets()
{
    g.changed = false;
    RefreshItemGuids();
    PrepareUserLibraries();
    const SoundStore& store = GetSoundStore(nullptr);
    CurrentLib();
    ui::SectionOpts opts;
    opts.foldable = false;
    opts.right_w = ui::ControlH();
    ui::Section sec;
    const bool  open = ui::BeginSection(sec, "Libraries", opts);
    ui::BeginSectionHeaderRight(sec);
    // 13-7 (audit B2 / C1.1): the mockup's .colbtn, a ghost glyph button.
    if (ui::IconOnlyButton("##collapse1", "\xC2\xAB", "Collapse (Ctrl+B)")) SetLibraryColumn1Collapsed(true);
    ui::EndSectionHeaderRight(sec);
    if (open) {
        std::string                        to_delete, add_file;
        const std::vector<UserLibraryFile> files = UserFilesNotInProject();
        bool                               any_project = false, any_user = !files.empty();
        for (const SoundLibrary& lib : store.libraries) (lib.user ? any_user : any_project) = true;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, ui::Dp(1.0f)));
        for (int user = 0; user < 2 && !g.changed; ++user) {
            if (!(user ? any_user : any_project)) continue;
            const std::string folder_tip = user ? "Folder: " + UserLibraryFolderText() : std::string();
            ListCaption(user ? "User" : "Project", user ? folder_tip.c_str() : nullptr);
            for (const SoundLibrary& lib : store.libraries) {
                if (lib.user != (user == 1)) continue;
                LibraryListRow(store, lib, &to_delete);
                if (g.changed) break;
            }
            if (user && !g.changed)
                for (const UserLibraryFile& f : files) UserFileRow(f, &add_file);
        }
        ImGui::PopStyleVar();
        if (!to_delete.empty())
            Later([to_delete] { Gesture("Delete library", [&](SoundStore& s) { return DeleteLibrary(s, to_delete); }); });
        if (!add_file.empty()) Later([add_file] { AddUserLibraryFile(add_file); });
        if (!any_project && !any_user) ui::SubText("No library yet");
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        // .two: the two add buttons share the width (6 px apart); Import under them, full width.
        const float avail = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        const float sp = ui::Dp(6.0f);
        const float half = std::max(1.0f, std::floor((avail - sp) * 0.5f));
        if (ui::SolidButton("+ Library", ImVec2(half, 0.0f)))
            Later([] {
                std::string uid;
                if (Gesture("Add library", [&](SoundStore& s) { return NewLibrary(s, &uid); })) PickLibrary(uid);
            });
        ImGui::SameLine(0.0f, sp);
        if (ui::SolidButton("+ User Library", ImVec2(std::max(1.0f, avail - half - sp), 0.0f))) Later([] { NewUserLibrary(); });
        ui::ItemTipText("A library saved in its own file in the User Library folder, for every project");
        if (ui::SolidButton("Import .ravlib...", ImVec2(avail, 0.0f))) RequestUserLibraryImport();
        ui::ItemTipText("Add a .ravlib file: it is copied into the User Library folder");
    }
    ui::EndSection(sec);
}

}  // namespace

void DrawLibrariesSection()
{
    LibrariesSectionWidgets();
    g_picks.Run();  // the gestures picked above, the list drawn
}

namespace {

// The rename field of a Switch Groups row, at x on a 28 px row; returns InlineName's result.
int GroupRowRename(float x)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImGui::SetCursorScreenPos(ImVec2(x, std::floor(p.y + (ui::RowH() - ui::ControlH()) * 0.5f)));
    const int res = InlineName(p.x + w - x - ui::Dp(4.0f));
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ui::RowH()));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    return res;
}

// The × of a Switch Groups row (.more, 22 x 20) at the row's right end, shown while it is hovered.
bool RowCross(ImVec2 p, float w, const char* tip)
{
    const float bw = ui::Dp(22.0f), bh = ui::Dp(20.0f);
    bool        hov = false;
    const bool  clicked = RowToolButton("##x", ImVec2(p.x + w - ui::Dp(4.0f) - bw, std::floor(p.y + (ui::RowH() - bh) * 0.5f)),
                                        bw, bh, false, &hov);
    CentredGlyph(ImGui::GetWindowDrawList(), ui::FontTitle(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                 hov ? ui::kText : ui::kMuted, "\xC3\x97");
    ui::ItemTipText(tip);
    return clicked;
}

// 13-7a (audit C2): a fixed "Switch Groups" section, "+ Group" at its header's right; group rows
// (.trow.grp, 28 px: chevron, folder icon, name, value count in faint, × on hover), their value
// rows one level in (18 px; the value dot, name, × on hover), then a dashed "+ value" field.
void SwitchGroupsSectionWidgets()
{
    g.changed = false;
    const SoundStore& store = GetSoundStore(nullptr);
    // A group / value rename whose target is gone (undo) ends, so it never blocks the tree keys.
    if (g.rename == RenameKind::Group || g.rename == RenameKind::Value) {
        const SwitchGroup* rg = FindGroup(store, g.r_a);
        bool alive = rg != nullptr;
        if (rg && g.rename == RenameKind::Value)
            alive = std::any_of(rg->values.begin(), rg->values.end(), [](const SwitchValue& v) { return v.uid == g.r_b; });
        if (!alive) EndRename();
    }
    ui::SectionOpts opts;
    opts.foldable = false;
    opts.right_w = ButtonWidth("+ Group");
    // 13-14 (C2.4): the card fills the rest of the column (.secx.sg { flex:1; min-height:140px }), its
    // body scrolling (the theme's fill_h body, begun after the header's "+ Group").
    opts.fill_h = true;
    opts.fill_min_h = 96.0f;
    ui::Section sec;
    const bool  open_sec = ui::BeginSection(sec, "Switch Groups", opts);
    ui::BeginSectionHeaderRight(sec);
    if (ui::SolidButton("+ Group"))
        Later([] {
            std::string gid;
            if (!Gesture("Add group", [&](SoundStore& s) { return AddGroup(s, "Group", &gid); })) return;
            g.rename = RenameKind::Group;
            g.r_a = gid;
            std::snprintf(g.buf, sizeof(g.buf), "%s",
                          FindGroup(GetSoundStore(nullptr), gid) ? FindGroup(GetSoundStore(nullptr), gid)->name.c_str() : "Group");
            g.r_focus = true;
        });
    ui::EndSectionHeaderRight(sec);
    if (open_sec && !g.changed) {
        std::string  del_group, del_value_group, del_value, add_group, add_name;
        // 13-18: a group default to set (follow: -1 none, 0 keep the Switches' own defaults, 1 clear them).
        std::string  def_group, def_value;
        int          def_follow = -1, def_count = 0;
        bool         sgd_drawn = false;  // the pending band's value row was drawn this frame
        ImDrawList*  dl = ImGui::GetWindowDrawList();
        const float  rh = ui::RowH(), gap = ui::Dp(6.0f), cs = ui::Dp(18.0f), is = ui::Dp(16.0f);
        const ImVec2 spacing0 = ImGui::GetStyle().ItemSpacing;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        for (const SwitchGroup& gr : store.groups) {
            ImGui::PushID(gr.uid.c_str());
            const ImGuiID open_id = ImGui::GetID("##grp");
            const bool    open = ImGui::GetStateStorage()->GetBool(open_id, true);
            const ImVec2  p = ImGui::GetCursorScreenPos();
            const float   w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
            const float   cy = p.y + rh * 0.5f;
            const float   x0 = p.x + ui::Dp(kRowPad);
            const float   nx = x0 + cs + gap + is + gap;
            if (g.rename == RenameKind::Group && g.r_a == gr.uid) {
                const int res = GroupRowRename(nx);
                if (res != 0) {
                    const std::string name = g.buf, uid = gr.uid;
                    EndRename();
                    if (res > 0) Later([uid, name] { Gesture("Rename group", [&](SoundStore& s) { return RenameGroup(s, uid, name); }); });
                }
                if (g.changed) {
                    ImGui::PopID();
                    break;
                }
            } else {
                ImGui::SetNextItemAllowOverlap();
                const bool cur = g.sg_gid == gr.uid && g.sg_vid.empty();
                if (ui::SelectRow("##grprow", cur, 0, rh, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap)) {
                    g.sg_gid = gr.uid;
                    g.sg_vid.clear();
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    g.sg_gid = gr.uid;
                    g.sg_vid.clear();
                    g.sg_menu = true;
                }
                const bool hovered = ui::SelectRowHovered();
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    g.rename = RenameKind::Group;
                    g.r_a = gr.uid;
                    std::snprintf(g.buf, sizeof(g.buf), "%s", gr.name.c_str());
                    g.r_focus = true;
                }
                ImGui::SetCursorScreenPos(ImVec2(x0, std::floor(cy - cs * 0.5f)));
                if (ui::Chevron("##chev", open, cs)) ImGui::GetStateStorage()->SetBool(open_id, !open);
                // The mockup's group row: a folder icon (ICON.group) before the name.
                DrawLibraryGlyph(dl, ImVec2(x0 + cs + gap, std::floor(cy - is * 0.5f)), is, kGlyphGroup, ui::kMuted);
                float rx = p.x + w - ui::Dp(4.0f);
                if (hovered) {
                    if (RowCross(p, w, "Delete the group: every Switch on it becomes a Random, its sounds kept")) del_group = gr.uid;
                    rx -= ui::Dp(22.0f) + gap;
                }
                // .rt.dim: the value count, 12 px faint.
                const std::string cnt = std::to_string(gr.values.size());
                const float       cw = ImGui::CalcTextSize(cnt.c_str()).x;
                if (rx - cw > nx + ui::Dp(16.0f)) {
                    rx -= cw;
                    dl->AddText(ImVec2(rx, std::floor(cy - ImGui::GetTextLineHeight() * 0.5f)), ui::kFaint, cnt.c_str());
                    rx -= gap;
                }
                ui::FontScope f(ui::FontTitle());
                ui::EllipsisTextTip(dl, ImVec2(nx, std::floor(cy - ImGui::GetTextLineHeight() * 0.5f)), rx, ui::kText, gr.name);
                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rh));
                ImGui::Dummy(ImVec2(0.0f, 0.0f));
            }
            if (open && !g.changed) {
                const float vx0 = p.x + ui::Dp(kRowPad + kIndent);
                const float vnx = vx0 + cs + gap + is + gap;
                // 13-18 D2: the value that plays for a Switch with no default of its own: the group's
                // default (its "default" tag), else the first value (the tag faint).
                const bool gdef_set = std::any_of(gr.values.begin(), gr.values.end(),
                                                  [&](const SwitchValue& x) { return x.uid == gr.default_value; });
                const std::string gdef = gdef_set ? gr.default_value : gr.values.empty() ? std::string() : gr.values.front().uid;
                // Values listed A to Z (case-insensitive); the group's stored order is unchanged.
                std::vector<const SwitchValue*> sorted;
                for (const SwitchValue& sv : gr.values) sorted.push_back(&sv);
                std::stable_sort(sorted.begin(), sorted.end(), [](const SwitchValue* a, const SwitchValue* b) {
                    return std::lexicographical_compare(a->name.begin(), a->name.end(), b->name.begin(), b->name.end(),
                                                        [](char x, char y) {
                                                            return std::tolower(static_cast<unsigned char>(x)) <
                                                                   std::tolower(static_cast<unsigned char>(y));
                                                        });
                });
                for (const SwitchValue* vp_ : sorted) {
                    const SwitchValue& v = *vp_;
                    ImGui::PushID(v.uid.c_str());
                    const ImVec2 vp = ImGui::GetCursorScreenPos();
                    const float  vcy = vp.y + rh * 0.5f;
                    if (g.rename == RenameKind::Value && g.r_a == gr.uid && g.r_b == v.uid) {
                        const int res = GroupRowRename(vnx);
                        if (res != 0) {
                            const std::string name = g.buf, gid = gr.uid, vid = v.uid;
                            EndRename();
                            if (res > 0)
                                Later([gid, vid, name] {
                                    Gesture("Rename value", [&](SoundStore& s) { return RenameValue(s, gid, vid, name); });
                                });
                        }
                        if (g.changed) {
                            ImGui::PopID();
                            break;
                        }
                    } else {
                        ImGui::SetNextItemAllowOverlap();
                        const bool cur = g.sg_gid == gr.uid && g.sg_vid == v.uid;
                        if (ui::SelectRow("##val", cur, 0, rh, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap)) {
                            g.sg_gid = gr.uid;
                            g.sg_vid = v.uid;
                        }
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                            g.sg_gid = gr.uid;
                            g.sg_vid = v.uid;
                            g.sg_menu = true;
                        }
                        const bool hovered = ui::SelectRowHovered();
                        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            g.rename = RenameKind::Value;
                            g.r_a = gr.uid;
                            g.r_b = v.uid;
                            std::snprintf(g.buf, sizeof(g.buf), "%s", v.name.c_str());
                            g.r_focus = true;
                        }
                        // The mockup's value row: the chevron's empty room, a dot (ICON.value), the name.
                        DrawLibraryGlyph(dl, ImVec2(vx0 + cs + gap, std::floor(vcy - is * 0.5f)), is, kGlyphValue, ui::kMuted);
                        float rx = vp.x + w - ui::Dp(4.0f);
                        if (hovered) {
                            if (RowCross(vp, w, "Remove the value: its sounds go to Unassigned in every Switch on this group")) {
                                del_value_group = gr.uid;
                                del_value = v.uid;
                            }
                            rx -= ui::Dp(22.0f) + gap;
                        }
                        if (v.uid == gdef) {
                            // The tree's "default" tag (.dtag); faint while no default is set.
                            const char* t = "default";
                            const float tw = ui::SmallTagWidth(t);
                            if (rx - tw > vnx + ui::Dp(16.0f)) {
                                rx -= tw;
                                if (!gdef_set) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
                                ui::DrawSmallTag(dl, ImVec2(rx, std::floor(vcy - ui::SmallTagHeight() * 0.5f)), t);
                                if (!gdef_set) ImGui::PopStyleVar();
                                ImGui::SetCursorScreenPos(ImVec2(rx, vp.y));
                                ImGui::Dummy(ImVec2(tw, rh));
                                ui::ItemTipText(gdef_set ? "The group's default value: plays for every Switch on this group that has no "
                                                           "default of its own"
                                                         : "No default set: the first value plays for every Switch on this group that "
                                                           "has no default of its own. Right-click a value to make it the default");
                                rx -= gap;
                            }
                        }
                        if (!v.roles.empty()) {
                            // 13-23 (D9): its roles, muted ("← toe, toe end"): their events get this value's part.
                            std::string rl = "\xE2\x86\x90 ";
                            for (size_t k = 0; k < v.roles.size(); ++k) rl += (k ? ", " : "") + RoleKeyLabel(v.roles[k]);
                            const float tw = std::min(ImGui::CalcTextSize(rl.c_str()).x + 1.0f, (rx - vnx) * 0.5f);
                            if (tw > ui::Dp(24.0f)) {
                                ui::EllipsisTextTip(dl, ImVec2(rx - tw, std::floor(vcy - ImGui::GetTextLineHeight() * 0.5f)), rx, ui::kMuted, rl,
                                                    "Events with these roles get this value's part, in every sound divided by the group "
                                                    "(right-click \xE2\x80\xBA Roles)");
                                rx -= tw + gap;
                            }
                        }
                        ui::FontScope f(ui::FontTitle());
                        ui::EllipsisTextTip(dl, ImVec2(vnx, std::floor(vcy - ImGui::GetTextLineHeight() * 0.5f)), rx, ui::kText, v.name);
                        ImGui::SetCursorScreenPos(ImVec2(vp.x, vp.y + rh));
                        ImGui::Dummy(ImVec2(0.0f, 0.0f));
                    }
                    if (!g.changed && g.sgd_gid == gr.uid && g.sgd_vid == v.uid) {
                        // 13-18 D3: the in-place confirm under the value row.
                        sgd_drawn = true;
                        const int n = CountDefaultOverrides(store, gr.uid, v.uid);
                        if (n == 0) {  // nothing left to ask (undo)
                            g.sgd_gid.clear();
                            g.sgd_vid.clear();
                        } else {
                            const ImVec2 bp = ImGui::GetCursorScreenPos();
                            ImGui::SetCursorScreenPos(ImVec2(vx0, bp.y + ui::Dp(2.0f)));
                            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, spacing0);
                            ui::ConfirmBand band;
                            ui::BeginConfirmBand(band, std::max(ui::Dp(40.0f), bp.x + w - vx0 - ui::Dp(4.0f)));
                            char msg[512];
                            if (n == 1)
                                std::snprintf(msg, sizeof(msg), "1 Switch has its own default. Make it follow %s?", v.name.c_str());
                            else
                                std::snprintf(msg, sizeof(msg), "%d Switches have their own default. Make them follow %s?", n,
                                              v.name.c_str());
                            ui::ToneText(msg, ui::kText, true);
                            if (ui::ConfirmButton("Follow##sgdef")) def_follow = 1;
                            ui::ItemTipText("Their own defaults are cleared: they play the group's default");
                            SameLineIfFits(ButtonWidth("Keep theirs"));
                            if (ui::SolidButton("Keep theirs##sgdef")) def_follow = 0;
                            ui::ItemTipText("Only the group's default changes");
                            SameLineIfFits(ButtonWidth("Cancel"));
                            if (ui::SolidButton("Cancel##sgdef")) {
                                g.sgd_gid.clear();
                                g.sgd_vid.clear();
                            }
                            ui::EndConfirmBand(band);
                            ImGui::PopStyleVar();
                            if (def_follow >= 0) {
                                def_count = n;
                                def_group = gr.uid;
                                def_value = v.uid;
                                g.sgd_gid.clear();
                                g.sgd_vid.clear();
                            }
                            const ImVec2 ep = ImGui::GetCursorScreenPos();
                            ImGui::SetCursorScreenPos(ImVec2(bp.x, ep.y + ui::Dp(4.0f)));
                            ImGui::Dummy(ImVec2(0.0f, 0.0f));
                        }
                    }
                    ImGui::PopID();
                }
                if (!g.changed) {
                    // .trow.add: the dashed "+ value" field one level in, 170 px; Enter adds the
                    // value (one undo point) and keeps the field for the next one.
                    std::array<char, 128>& buf = g.add_value[gr.uid];
                    const ImVec2           ap = ImGui::GetCursorScreenPos();
                    ImGui::SetCursorScreenPos(ImVec2(vx0, std::floor(ap.y + (rh - ui::ControlH()) * 0.5f)));
                    if (g.add_value_focus == gr.uid) {
                        ImGui::SetKeyboardFocusHere();
                        g.add_value_focus.clear();
                    }
                    const float aw = std::min(ui::Dp(170.0f), std::max(ui::Dp(40.0f), ap.x + w - vx0 - ui::Dp(4.0f)));
                    if (ui::AddField("##addval", "+ value", buf.data(), buf.size(), aw, ImGuiInputTextFlags_EnterReturnsTrue)) {
                        std::string name = buf.data();
                        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
                        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.erase(name.begin());
                        if (!name.empty()) {
                            add_group = gr.uid;
                            add_name = name;
                        }
                    }
                    ui::ItemTipText("Type a value's name, then Enter");
                    ImGui::SetCursorScreenPos(ImVec2(ap.x, ap.y + rh));
                    ImGui::Dummy(ImVec2(0.0f, 0.0f));
                }
            }
            ImGui::PopID();
            if (g.changed || !add_group.empty()) break;
        }
        ImGui::PopStyleVar();
        // 13-18: a pending band whose value row was not drawn (its group folded) is dropped, so it
        // never comes back later out of context.
        if (!g.changed && add_group.empty() && !sgd_drawn) {
            g.sgd_gid.clear();
            g.sgd_vid.clear();
        }
        // Antho 2026-10-12: the clicked row renames by F2 or its right-click menu (as the tree's rows), besides the
        // double-click; the menu also deletes it.
        {
            const SwitchGroup* cg = g.sg_gid.empty() ? nullptr : FindGroup(store, g.sg_gid);
            const SwitchValue* cv = nullptr;
            if (cg && !g.sg_vid.empty())
                for (const SwitchValue& v : cg->values)
                    if (v.uid == g.sg_vid) cv = &v;
            if (!cg || (!g.sg_vid.empty() && !cv)) {  // gone (deleted, undone)
                g.sg_gid.clear();
                g.sg_vid.clear();
            }
            const SwitchGroup* pg = g.sgd_gid.empty() ? nullptr : FindGroup(store, g.sgd_gid);
            if (!g.sgd_gid.empty() &&
                (!pg || std::none_of(pg->values.begin(), pg->values.end(), [](const SwitchValue& v) { return v.uid == g.sgd_vid; }))) {
                // its value went (undo)
                g.sgd_gid.clear();
                g.sgd_vid.clear();
            }
            auto start_rename = [&]() {
                g.rename = cv ? RenameKind::Value : RenameKind::Group;
                g.r_a = cg->uid;
                g.r_b = cv ? cv->uid : std::string();
                std::snprintf(g.buf, sizeof(g.buf), "%s", cv ? cv->name.c_str() : cg->name.c_str());
                g.r_focus = true;
            };
            if (cg && !g.changed && g.rename == RenameKind::None && !ImGui::GetIO().WantTextInput &&
                ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
                ImGui::IsKeyPressed(ImGuiKey_F2, false))
                start_rename();
            if (g.sg_menu) {
                g.sg_menu = false;
                ImGui::OpenPopup("##sgmenu");
            }
            if (ui::BeginMenuPopup("##sgmenu")) {
                if (!cg) ImGui::CloseCurrentPopup();  // its row went (undo)
                if (cg && ImGui::MenuItem("Rename", "F2", false, !g.changed && g.rename == RenameKind::None)) start_rename();
                if (cg && !cv && ImGui::MenuItem("Delete group")) del_group = cg->uid;
                if (cg && cv) {
                    // 13-18 D2 / D3: the group's default; asks first when Switches have their own.
                    const bool is_def = cg->default_value == cv->uid;
                    if (ImGui::MenuItem("Make default", nullptr, is_def, !is_def)) {
                        if (CountDefaultOverrides(store, cg->uid, cv->uid) == 0) {
                            def_group = cg->uid;
                            def_value = cv->uid;
                            def_follow = 0;
                        } else {
                            g.sgd_gid = cg->uid;
                            g.sgd_vid = cv->uid;
                        }
                    }
                    ui::ItemTipText("Plays for every Switch on this group that has no default of its own");
                    // 13-23 (D9): Roles ▸ -- which events (by role) get this value's part in every sound divided
                    // by the group. A role belongs to one value: one used by another shows "(→ Back)" and moves.
                    ui::MenuSeparator();
                    if (ui::BeginSubMenu("Roles")) {
                        const std::vector<std::string>& keys = LibraryRoleKeys();
                        const size_t                    nb = std::min(LibraryBuiltinRoleCount(), keys.size());
                        ui::MenuCaption(("Roles \xE2\x86\x92 " + cv->name).c_str());
                        for (size_t i = 0; i < keys.size(); ++i) {
                            if (i == nb) {
                                ui::MenuSeparator();
                                ui::MenuCaption("Custom roles");
                            }
                            const std::string& key = keys[i];
                            const SwitchValue* owner = nullptr;
                            for (const SwitchValue& o : cg->values)
                                if (std::find(o.roles.begin(), o.roles.end(), key) != o.roles.end()) owner = &o;
                            const bool        on = owner == cv;
                            const std::string hint = owner && !on ? "(\xE2\x86\x92 " + owner->name + ")" : std::string();
                            const std::string label = RoleKeyLabel(key) + "##role" + key;
                            if (ui::MenuEntry(label.c_str(), hint.empty() ? nullptr : hint.c_str(), on)) {
                                const std::string gid = cg->uid, vid = cv->uid, gname = cg->name, vname = cv->name;
                                const std::string toast = on ? gname + ": " + RoleKeyLabel(key) + " removed from " + vname
                                                             : gname + ": " + RoleKeyLabel(key) + " \xE2\x86\x92 " + vname +
                                                                   (owner ? " (moved from " + owner->name + ")" : std::string());
                                Later([gid, vid, key, toast] {
                                    Gesture("Roles", [&](SoundStore& s) { return ToggleGroupRole(s, gid, vid, key); }, toast);
                                });
                            }
                        }
                        ui::ToneText("One undo point per change. A role belongs to one value of the group.", ui::kFaint, true, ui::Dp(200.0f));
                        ImGui::EndMenu();
                    }
                }
                if (cg && cv) ui::MenuSeparator();
                if (cg && cv && ImGui::MenuItem("Remove value")) {
                    del_value_group = cg->uid;
                    del_value = cv->uid;
                }
                ui::EndMenuPopup();
            }
        }
        if (!g.changed && def_follow >= 0 && !def_group.empty()) {
            const SwitchGroup* dg = FindGroup(store, def_group);
            std::string        vname;
            if (dg)
                for (const SwitchValue& v : dg->values)
                    if (v.uid == def_value) vname = v.name;
            std::string toast = vname + " is the group's default";
            if (def_follow == 1)
                toast += def_count == 1 ? " \xC2\xB7 1 Switch follows it" : " \xC2\xB7 " + std::to_string(def_count) + " Switches follow it";
            Later([def_group, def_value, def_follow, toast] {
                Gesture(
                    "Make group default",
                    [&](SoundStore& s) { return SetGroupDefault(s, def_group, def_value, def_follow == 1); }, toast);
            });
        }
        if (!g.changed && !del_group.empty())
            Later([del_group] { Gesture("Delete group", [&](SoundStore& s) { return DeleteGroup(s, del_group); }); });
        if (!g.changed && !del_value.empty())
            Later([del_value_group, del_value] {
                Gesture("Remove value", [&](SoundStore& s) { return DeleteValue(s, del_value_group, del_value); });
            });
        if (!g.changed && !add_group.empty())
            Later([add_group, add_name] {
                std::string vid;
                if (!Gesture("Add value", [&](SoundStore& s) { return AddValue(s, add_group, add_name, &vid); })) return;
                g.add_value[add_group].fill('\0');
                g.add_value_focus = add_group;  // type the next one
            });
        if (!g.changed && store.groups.empty()) ui::SubText("No Switch Group yet");
    }
    ui::EndSection(sec);  // ends the body too
}

}  // namespace

void DrawSwitchGroupsSection()
{
    SwitchGroupsSectionWidgets();
    g_picks.Run();  // the gestures picked above, the groups drawn
}

// ---- Column 2 -----------------------------------------------------------------------------------

namespace {

// 13-7a (audit C3.1): the tree's header (.pane-h, 28 px): the library's 8 px dot, its name as an
// inline title (14 px 600, transparent until hovered: typing renames it, one undo point), the ▾
// library switcher while column 1 is collapsed, then the ⋯ menu (Export / Locate / Refresh for a
// User Library, Delete).
void TreeHeader(const SoundLibrary& lib)
{
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float  hh = ui::Dp(28.0f), gap = ui::Dp(8.0f), bs = ui::ControlH();
    const bool   switcher = LibraryColumn1Collapsed();
    dl->AddCircleFilled(ImVec2(p.x + ui::Dp(4.0f), p.y + hh * 0.5f), ui::Dp(4.0f), LibDot(lib.color));
    const float tx = p.x + ui::Dp(8.0f) + gap;
    const float right = (switcher ? bs + ui::Dp(4.0f) : 0.0f) + bs + gap;
    const float tw = std::max(ui::Dp(40.0f), p.x + w - tx - right);
    const ImGuiID title_id = ImGui::GetID("##libtitle");
    if (ImGui::GetActiveID() != title_id) {  // shows the name, unless it is being typed
        g.title_lib = lib.uid;
        std::snprintf(g.title, sizeof(g.title), "%s", lib.name.c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(tx, p.y));
    ui::TitleField("##libtitle", g.title, sizeof(g.title), tw);
    if (ImGui::IsItemDeactivated()) {
        // The library typed into (another may be shown by now: a click in column 1 ended the typing).
        std::string         name = g.title;
        const std::string   uid = g.title_lib;
        const SoundLibrary* typed = FindLibrary(GetSoundStore(nullptr), uid);
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
        if (typed && !name.empty() && name != typed->name)
            Later([uid, name] { Gesture("Rename library", [&](SoundStore& s) { return RenameLibrary(s, uid, name); }); });
        g.title_lib.clear();
        if (g.changed) {
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + hh));
            return;
        }
    }
    const float by = std::floor(p.y + (hh - bs) * 0.5f);
    float       bx = p.x + w - bs;
    if (switcher) {
        ImGui::SetCursorScreenPos(ImVec2(bx - bs - ui::Dp(4.0f), by));
        if (ui::IconOnlyButton("##libswitch", "\xE2\x96\xBE", "Show another library")) ImGui::OpenPopup("##libswitchmenu");
        if (ui::BeginMenuPopup("##libswitchmenu")) {
            ui::MenuCaption("Libraries");
            for (const SoundLibrary& l : GetSoundStore(nullptr).libraries) {
                ImGui::PushID(l.uid.c_str());
                if (ImGui::MenuItem(l.name.c_str(), nullptr, l.uid == g.lib)) PickLibrary(l.uid);
                ImGui::PopID();
            }
            ui::EndMenuPopup();
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(bx, by));
    if (ui::IconOnlyButton("##libmore", "\xE2\x8B\xAF", "Library menu", 0.0f, ImGui::IsPopupOpen("##libheadmenu")))
        ImGui::OpenPopup("##libheadmenu");
    std::string to_delete;
    if (ui::BeginMenuPopup("##libheadmenu")) {
        LibraryMenuItems(lib, &to_delete, false);
        ui::EndMenuPopup();
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + hh));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    if (!to_delete.empty())
        Later([to_delete] { Gesture("Delete library", [&](SoundStore& s) { return DeleteLibrary(s, to_delete); }); });
}

void LibraryTreeWidgets()
{
    g.changed = false;
    g.tree_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);  // column 2 and its tree
    if (g.tree_focused) {  // the Switch Groups row picked last lets go (its F2 / highlight)
        g.sg_gid.clear();
        g.sg_vid.clear();
    }
    RefreshItemGuids();
    const SoundLibrary* lib = CurrentLib();
    if (!lib) {
        ui::SubText("No library yet");
        if (ui::SolidButton("+ Library##tree"))
            Later([] {
                std::string uid;
                if (Gesture("Add library", [&](SoundStore& s) { return NewLibrary(s, &uid); })) g.lib = uid;
            });
        return;
    }
    TreeHeader(*lib);
    lib = CurrentLib();
    if (!lib || g.changed) return;
    // 13-7a (audit C3.2): the toolbar in its well; the pane's gap under it (no separator).
    Toolbar(*lib);
    if (!g.changed) TreeKeys(*lib);  // after a toolbar gesture `lib` is stale
    lib = CurrentLib();  // a gesture above rewrote the store
    if (!lib) return;
    // 13-17c: the search field under the toolbar filters the tree as you type.
    {  // Antho 2026-10-13: an × at its right clears it
        const float bs = ui::ControlH(), sp = ImGui::GetStyle().ItemSpacing.x;
        ui::TextField("##treesearch", "Search nodes", g.search, sizeof g.search,
                      std::max(ui::Dp(40.0f), ImGui::GetContentRegionAvail().x - bs - sp), ImGuiInputTextFlags_EscapeClearsAll);
        ImGui::SameLine(0.0f, sp);
        ImGui::BeginDisabled(g.search[0] == '\0');
        if (ui::IconOnlyButton("##treesearchx", "\xE2\x9C\x95", "Clear the search (Esc)")) g.search[0] = '\0';
        ImGui::EndDisabled();
    }
    const std::string search = g.search;

    // Antho 2026-10-12: every container starts open (the whole hierarchy shows); ▶ folds one. BuildTreeRows takes
    // the open set: every container not folded ("!root" marks a folded root).
    std::set<std::string> open;
    for (const SoundNode& n : lib->nodes)
        if (n.type != SoundNodeType::Sound && !g.closed.count(n.uid)) open.insert(n.uid);
    if (g.closed.count(lib->root)) open.insert("!root");
    g.rows = BuildTreeRows(GetSoundStore(nullptr), *lib, open, search);
    // 13-17b: a node that just appeared (added, duplicated, dropped, brought back by an undo) is
    // scrolled into view, just enough (DrawRow); a library switch never scrolls.
    {
        std::set<std::string> now;
        for (const SoundNode& n : lib->nodes) now.insert(n.uid);
        if (g.known_lib == lib->uid && now != g.known_nodes)
            for (const TreeRow& r : g.rows)
                if (!r.node.empty() && !g.known_nodes.count(r.node)) {
                    g.scroll_to = r.row;
                    g.has_scroll_to = true;
                    break;
                }
        g.known_lib = lib->uid;
        g.known_nodes = std::move(now);
    }
    // The selection is pruned against the rows shown (an undo may remove uids; 13-17c: a search
    // keeps only the rows it shows, so Delete / Cut never act on a hidden row).
    g.sel.erase(std::remove_if(g.sel.begin(), g.sel.end(),
                               [&](const LibraryRow& s) {
                                   for (const TreeRow& r : g.rows)
                                       if (r.row == s && (r.kind == TreeRowKind::Node || r.kind == TreeRowKind::Value))
                                           return false;
                                   return true;
                               }),
                g.sel.end());
    // A row rename whose row is no longer shown (collapsed, undone, other library) ends, so the
    // tree keys are not blocked by an invisible field.
    if (g.rename == RenameKind::Row) {
        bool shown = g.r_a == lib->uid;
        if (shown) {
            shown = false;
            for (const TreeRow& r : g.rows)
                if (r.row == g.r_row && (r.kind == TreeRowKind::Node || r.kind == TreeRowKind::Value)) shown = true;
        }
        if (!shown) EndRename();
    }

    if (ImGui::BeginChild("##treerows", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None)) {
        if (!ImGui::IsAnyItemActive() && !ImGui::GetDragDropPayload() && ImGui::IsWindowHovered() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
            g.sel.clear();
        // The tree's rows touch (.tree gap 0).
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        g.row_menu_open = ImGui::IsPopupOpen("##rowmenu");
        const std::vector<TreeRow> rows = g.rows;  // a gesture may rebuild g.rows
        g.gaps.clear();
        g.hidden_gaps.clear();
        std::set<std::string> shown;
        for (const TreeRow& r : rows)
            if (!r.node.empty()) shown.insert(r.node);
        for (const LibraryGap& gp : CheckLibrary(GetSoundStore(nullptr), *lib).warnings) {
            std::string& t = g.gaps[gp.node];
            t += (t.empty() ? "" : "\n") + gp.reason;
            if (!shown.count(gp.node)) g.hidden_gaps.push_back(gp);
        }
        for (size_t i = 0; i < rows.size() && !g.changed; ++i) {
            const SoundLibrary* cur = FindLibrary(GetSoundStore(nullptr), g.lib);
            if (!cur) break;
            DrawRow(*cur, rows[i], i);
        }
        ImGui::PopStyleVar();
        lib = FindLibrary(GetSoundStore(nullptr), g.lib);
        const SoundNode* root = lib ? FindNode(*lib, lib->root) : nullptr;
        if (!search.empty() && rows.empty()) ui::SubText("No node matches");  // 13-17c
        else if (!g.changed && root && root->type != SoundNodeType::Switch && ChildrenOf(*lib, root->uid).empty())
            EmptyLibraryHint(*lib);
        lib = FindLibrary(GetSoundStore(nullptr), g.lib);
        if (!g.changed && lib && FindNode(*lib, lib->root)) EmptySpaceDropTarget(*lib);
        if (!ImGui::GetDragDropPayload()) g.hover_uid.clear();
        if (g.open_row_menu) {
            ImGui::OpenPopup("##rowmenu");
            g.open_row_menu = false;
        }
        if (const SoundLibrary* cur = FindLibrary(GetSoundStore(nullptr), g.lib)) RowMenu(*cur);
        // 13-17b: a gesture's frame stops drawing rows; the tree keeps its last full height so ImGui
        // does not clamp the scroll to the rows drawn (the tree jumped to the top).
        const float y = ImGui::GetCursorPosY();
        if (!g.changed) g.rows_h = y;
        else if (g.rows_h > y) ImGui::Dummy(ImVec2(0.0f, g.rows_h - y));
    }
    ImGui::EndChild();
}

// ---- Column 3: several rows selected ------------------------------------------------------------

// 13-7a (audit C6): the title "N rows selected" (15 / 600) and what they are, "Make them default"
// above the card, then the fixed "Change them all at once · one undo point" card: Type (with
// "mixed" in warn), the "Switch on" select, "Put into".
void LibraryBatchPanelWidgets()
{
    g.changed = false;
    const SoundStore& store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, g.lib);
    if (!lib) return;
    const std::vector<LibraryRow> rows = g.sel;
    // The common type of the selected containers (-1 = mixed or none).
    int common = -2;
    std::string common_group;
    int values = 0, containers = 0, sounds = 0, empty_values = 0;
    for (const LibraryRow& r : rows) {
        if (RowIsValue(*lib, r)) ++values;
        if (const SoundNode* shown = FindNode(*lib, RowNode(*lib, r)))
            (shown->type == SoundNodeType::Sound ? sounds : containers) += 1;
        else
            ++empty_values;
        // Bug 3: a Switch's value rows (a select-all) act on their Switch, as SetRowsType does.
        const SoundNode* n = FindNode(*lib, RowTypeNode(*lib, r));
        if (!n || n->type == SoundNodeType::Sound) continue;
        const int t = n->type == SoundNodeType::Random ? 0 : n->type == SoundNodeType::Blend ? 1 : 2;
        if (common == -2) {
            common = t;
            common_group = n->group_uid;
        } else if (common != t || (t == 2 && common_group != n->group_uid)) {
            common = -1;
        }
    }
    {
        const std::string title = std::to_string(rows.size()) + " rows selected";
        ui::FontScope     f(ui::FontPaneTitle());
        const ImVec2      p = ImGui::GetCursorScreenPos();
        const float       w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight()));
        ui::EllipsisTextTip(ImGui::GetWindowDrawList(), p, p.x + w, ui::kText, title);
    }
    std::string sum;
    auto part = [&](int count, const char* one, const char* many) {
        if (count <= 0) return;
        if (!sum.empty()) sum += " \xC2\xB7 ";
        sum += std::to_string(count) + " " + (count == 1 ? one : many);
    };
    part(containers, "container", "containers");
    part(sounds, "sound", "sounds");
    part(empty_values, "empty value", "empty values");
    if (!sum.empty()) ui::SubText(sum.c_str());
    if (values == 0) ImGui::BeginDisabled();
    if (ui::SolidButton("Make them default (one per Switch)")) Later([rows] { DoMakeDefault(rows); });
    if (values == 0) ImGui::EndDisabled();
    ui::ItemTipText(values ? "One default per Switch" : "Select Switch value rows first");
    if (g.changed) return;
    ui::SectionOpts opts;
    opts.foldable = false;
    opts.suffix = "\xC2\xB7 one undo point";
    ui::Section sec;
    if (ui::BeginSection(sec, "Change them all at once", opts)) {
        static const char* const kTypes[3] = {"Random", "Blend", "Switch"};
        int sel = common >= 0 ? common : -1;
        ui::LabelRow("Type", ui::Dp(150.0f));
        const int was = sel;
        const ui::Icon kTypeIcons[3] = {TypeIcon(SoundNodeType::Random), TypeIcon(SoundNodeType::Blend),
                                        TypeIcon(SoundNodeType::Switch)};
        if (ui::Segmented("##batchtype", kTypes, 3, &sel, 0.0f, -1, kTypeIcons)) {
            if (sel == 0) Later([rows] { DoSetType(rows, SoundNodeType::Random, {}); });
            else if (sel == 1) Later([rows] { DoSetType(rows, SoundNodeType::Blend, {}); });
            else ImGui::OpenPopup("##batchswitch");
        } else if (was == 2 && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && ui::SegmentedHovered() == 2 &&
                   !ImGui::IsPopupOpen("##batchswitch")) {
            // Bug 3: all already Switch, the segment does not "change": it still picks the group.
            ImGui::OpenPopup("##batchswitch");
        }
        if (common == -1) {
            // The mockup's "mixed": 11 px in warn.
            ui::FontScope f(ui::FontSmall());
            SameLineIfFits(ui::TextWidth("mixed"));
            const float y = ImGui::GetCursorPosY();
            ImGui::SetCursorPosY(y + std::max(0.0f, (ui::ControlH() - ImGui::GetTextLineHeight()) * 0.5f));
            ui::ToneText("mixed", ui::kWarn);
        }
        if (ui::BeginMenuPopup("##batchswitch")) {
            ui::MenuCaption("Switch on");
            const std::string gid = GroupMenuItems(false, nullptr);
            if (!gid.empty()) Later([rows, gid] { BatchSwitchOn(rows, gid); });
            ui::EndMenuPopup();
        }
        const SwitchGroup* cg = common == 2 ? FindGroup(store, common_group) : nullptr;
        ui::LabelRow("Switch on", ui::Dp(160.0f));
        if (ui::BeginComboStyled("##batchswitch2", cg ? cg->name.c_str() : "Choose a Switch Group...", ui::Dp(160.0f))) {
            const std::string gid = GroupMenuItems(false, nullptr);
            if (!gid.empty()) Later([rows, gid] { BatchSwitchOn(rows, gid); });
            ui::EndComboStyled();
        }
        ui::ItemTipText("Also converts the Random and Blend containers");
        ui::LabelRow("Put into", MenuButtonWidth("Put into"));
        if (MenuButton("Put into")) ImGui::OpenPopup("##putinto");
        if (ui::BeginMenuPopup("##putinto")) {
            if (ui::IconMenuItem(TypeIcon(SoundNodeType::Random), "New Random")) Later([rows] { DoWrap(rows, SoundNodeType::Random, {}); });
            if (ui::IconMenuItem(TypeIcon(SoundNodeType::Blend), "New Blend")) Later([rows] { DoWrap(rows, SoundNodeType::Blend, {}); });
            if (ui::IconBeginMenu(TypeIcon(SoundNodeType::Switch), "New Switch on")) {
                const std::string gid = GroupMenuItems(false, nullptr);
                if (!gid.empty()) Later([rows, gid] { DoWrap(rows, SoundNodeType::Switch, gid); });
                ImGui::EndMenu();
            }
            ImGui::Separator();
            for (size_t ni = 0; !g.changed && ni < lib->nodes.size(); ++ni) {
                const SoundNode& n = lib->nodes[ni];
                if (n.type != SoundNodeType::Random && n.type != SoundNodeType::Blend) continue;
                DropTarget to;
                to.lib = lib->uid;
                to.row = RowOfNode(*lib, n.uid);
                to.mode = DropMode::Into;
                const bool ok = CanDrop(store, lib->uid, rows, to);
                ImGui::PushID(n.uid.c_str());
                if (ui::IconMenuItem(TypeIcon(n.type), n.name.c_str(), TypeWord(n.type), false, ok))
                    Later([lib_uid = lib->uid, rows, to, name = n.name] {
                        Gesture("Put into " + name, [&](SoundStore& s) { return MoveRows(s, lib_uid, rows, to, nullptr); });
                    });
                ImGui::PopID();
            }
            ui::EndMenuPopup();
        }
    }
    ui::EndSection(sec);
}

}  // namespace

void DrawLibraryTree()
{
    LibraryTreeWidgets();
    g_picks.Run();  // the gestures picked above (toolbar, rows, menus, drops, keys), the tree drawn
}

void DrawLibraryBatchPanel()
{
    LibraryBatchPanelWidgets();
    g_picks.Run();  // the gestures picked above, the panel drawn
}

// ---- Seams --------------------------------------------------------------------------------------

const std::string& SelectedLibraryUid()
{
    CurrentLib();
    return g.lib;
}

const std::vector<LibraryRow>& SelectedLibraryRows() { return g.sel; }

void SelectLibraryRow(const std::string& lib_uid, const LibraryRow& row)
{
    if (g.lib != lib_uid) g.has_anchor = false;
    g.lib = lib_uid;
    g.search[0] = '\0';  // 13-21: a search would prune the row picked here
    g.scroll_to = row;
    g.has_scroll_to = true;
    if (const SoundLibrary* lib = FindLibrary(GetSoundStore(nullptr), lib_uid)) {
        const std::string n = RowNode(*lib, row);
        if (!n.empty()) RevealNode(*lib, n);
    }
    SelectOnly(row);
}

std::string LibraryRowNode()
{
    if (g.sel.size() != 1) return {};
    const SoundLibrary* lib = FindLibrary(GetSoundStore(nullptr), g.lib);
    return lib ? RowNode(*lib, g.sel.front()) : std::string();
}

void SetLibraryTreeHooks(const LibraryTreeHooks& hooks) { g.hooks = hooks; }

bool LibraryTreeTakesArrows()
{
    return g.tree_focused && g.rename == RenameKind::None && !g.rows.empty();
}
const LibraryTreeHooks& GetLibraryTreeHooks() { return g.hooks; }

const std::vector<std::string>& LibraryRoleKeys()
{
    static std::vector<std::string> keys;
    static int                      frame = -1000;
    const int                       f = ImGui::GetFrameCount();
    if (keys.empty() || f - frame >= 60 || f < frame) {
        frame = f;
        std::vector<std::string> custom;
        for (const CustomRole& c : GetCustomRoles(RulesResourceRoot())) custom.push_back(c.key);
        keys = RoleMenuKeys(custom);
    }
    return keys;
}

size_t LibraryBuiltinRoleCount() { return RoleMenuKeys({}).size(); }

}  // namespace rav

#endif  // _WIN32
