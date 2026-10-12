// SPDX-License-Identifier: MIT
//
// See library_tree_ui.h. Every gesture here is one ModifySoundStore undo point ("RAV: <gesture>")
// followed by a toast offering Undo; the edits themselves are library_tree_edit.h (pure,
// host-tested).

#include "library_tree_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include "library_view_ui.h"
#include "reaper_api.h"
#include "sound_place.h"
#include "sound_project.h"
#include "sound_record.h"
#include "sound_switch.h"

namespace rav {
namespace {

constexpr float kIndent = 14.0f;
constexpr double kAutoOpenSeconds = 0.6;
constexpr const char kRowsPayload[] = "RAV_LIB_ROWS";

enum class RenameKind { None, Library, Group, Value, Row };

struct TreeState {
    std::string              lib;
    std::vector<LibraryRow>  sel;
    LibraryRow               anchor;
    bool                     has_anchor = false;
    std::set<std::string>    open;
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
    std::map<std::string, std::string> add_value_text;
    LibraryTreeHooks         hooks;
    // Missing items: the project's item GUIDs (upper case), rebuilt on a project change.
    int                      guid_scc = -1;
    ReaProject*              guid_proj = nullptr;
    // Column 2 (toolbar + tree) had the keyboard focus last frame: the tree keys act only then.
    bool                     tree_focused = false;
    std::set<std::string>    item_guids;
    // A gesture ran this frame: the store was rewritten, so every SoundLibrary / SoundNode
    // reference taken before it is stale; the drawing loops stop at once (next frame redraws).
    bool                     changed = false;
};
TreeState g;

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

ImU32 TypeColor(SoundNodeType t)
{
    switch (t) {
    case SoundNodeType::Sound:  return ui::kMuted;
    case SoundNodeType::Random: return ui::kCurvePalette[1];
    case SoundNodeType::Blend:  return ui::kCurvePalette[5];
    default:                    return ui::kCurvePalette[2];
    }
}

const char* TypeLetter(SoundNodeType t)
{
    switch (t) {
    case SoundNodeType::Sound:  return "~";
    case SoundNodeType::Random: return "R";
    case SoundNodeType::Blend:  return "B";
    default:                    return "S";
    }
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
        g.open.insert(n->parent);
        if (n->parent == lib.root) g.open.erase("!root");
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

void DrawChevron(ImDrawList* dl, ImVec2 c, bool open, ImU32 col)
{
    const float r = 3.5f;
    if (open)
        dl->AddTriangleFilled(ImVec2(c.x - r, c.y - r * 0.6f), ImVec2(c.x + r, c.y - r * 0.6f), ImVec2(c.x, c.y + r * 0.8f), col);
    else
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.6f, c.y - r), ImVec2(c.x - r * 0.6f, c.y + r), ImVec2(c.x + r * 0.8f, c.y), col);
}

void DrawTypeIcon(ImDrawList* dl, ImVec2 p, float s, SoundNodeType t, bool hot)
{
    const ImU32 col = TypeColor(t);
    dl->AddRect(p, ImVec2(p.x + s, p.y + s), hot ? ui::kText : col, ui::kRadiusSm * 0.6f);
    const char* l = TypeLetter(t);
    const ImVec2 ts = ImGui::CalcTextSize(l);
    dl->AddText(ImVec2(p.x + (s - ts.x) * 0.5f, p.y + (s - ts.y) * 0.5f), col, l);
}

// A SolidButton with a small down arrow after its label (a menu opens on click).
// Bug 1 (13-7a): a row of buttons wraps instead of being cut off when its column is narrow.
float ButtonWidth(const char* label)
{
    return ImGui::CalcTextSize(label, nullptr, true).x + 2.0f * ImGui::GetStyle().FramePadding.x;
}

float MenuButtonWidth(const char* label) { return ButtonWidth((std::string(label) + "    ").c_str()); }

// SameLine when the next item (`next_w` wide) still fits after the last one, else a new line.
void SameLineIfFits(float next_w)
{
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;  // the line's end
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + next_w <= right) ImGui::SameLine();
}

bool MenuButton(const char* label, bool enabled = true)
{
    std::string l = std::string(label) + "    ";
    if (!enabled) ImGui::BeginDisabled();
    const bool r = ui::SolidButton(l.c_str());
    if (!enabled) ImGui::EndDisabled();
    const ImVec2 mx = ImGui::GetItemRectMax(), mn = ImGui::GetItemRectMin();
    DrawChevron(ImGui::GetWindowDrawList(), ImVec2(mx.x - 9.0f, (mn.y + mx.y) * 0.5f), true,
                enabled ? ui::kMuted : ui::kFaint);
    return r;
}

void Tip(const std::string& text)
{
    if (!text.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", text.c_str());
}

// An inline name field: 1 = commit, -1 = cancel, 0 = still typing.
int InlineName(float width)
{
    ImGui::SetNextItemWidth(std::max(40.0f, width));
    if (g.r_focus) {
        ImGui::SetKeyboardFocusHere();
        g.r_focus = false;
    }
    const bool enter = ImGui::InputText("##rename", g.buf, sizeof(g.buf),
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

void DoPaste(const SoundLibrary& lib, const LibraryRow* row)
{
    if (g.clip.empty()) return;
    const DropTarget to = PasteTarget(lib, row);
    const std::string src = g.clip_lib;
    const std::vector<LibraryRow> rows = g.clip;
    if (!CanDrop(GetSoundStore(nullptr), src, rows, to)) {
        ShowLibraryToast("Cannot paste there", false);
        return;
    }
    const std::string lib_uid = lib.uid;  // `lib` is stale once the gesture rewrote the store
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
    if (ImGui::MenuItem("Random")) DoSetType(rows, SoundNodeType::Random, {});
    if (ImGui::MenuItem("Blend")) DoSetType(rows, SoundNodeType::Blend, {});
    if (ImGui::BeginMenu("Switch on")) {
        const std::string gid = GroupMenuItems(false, nullptr);
        if (!gid.empty()) BatchSwitchOn(rows, gid);
        ImGui::EndMenu();
    }
}

void WrapMenu(const std::vector<LibraryRow>& rows)
{
    if (ImGui::MenuItem("Random")) DoWrap(rows, SoundNodeType::Random, {});
    if (ImGui::MenuItem("Blend")) DoWrap(rows, SoundNodeType::Blend, {});
    if (ImGui::BeginMenu("Switch on")) {
        const std::string gid = GroupMenuItems(false, nullptr);
        if (!gid.empty()) DoWrap(rows, SoundNodeType::Switch, gid);
        ImGui::EndMenu();
    }
}

// ---- The row menu (right-click / ⋯) -------------------------------------------------------------

void RowMenu(const SoundLibrary& lib)
{
    if (!ImGui::BeginPopup("##rowmenu")) return;
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
        if (ImGui::MenuItem("Sound", nullptr, false, can_add(SoundNodeType::Sound, &why))) DoAdd(SoundNodeType::Sound, {}, false, &rows);
        Tip(why);
        why.clear();
        if (ImGui::MenuItem("Random", nullptr, false, can_add(SoundNodeType::Random, &why))) DoAdd(SoundNodeType::Random, {}, false, &rows);
        Tip(why);
        why.clear();
        if (ImGui::MenuItem("Blend", nullptr, false, can_add(SoundNodeType::Blend, &why))) DoAdd(SoundNodeType::Blend, {}, false, &rows);
        Tip(why);
        if (can_add(SoundNodeType::Switch, nullptr) && ImGui::BeginMenu("Switch on")) {
            bool ng = false;
            const std::string gid = GroupMenuItems(true, &ng);
            if (!gid.empty() || ng) DoAdd(SoundNodeType::Switch, gid, ng, &rows);
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
        if (!gid.empty()) BatchSwitchOn(rows, gid);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Rename", "F2", false, (has_node || is_value) && !g.changed)) StartRowRename(lib, row);
    const bool movable = has_node && node_uid != root_uid;
    if (ImGui::MenuItem("Cut", "Ctrl+X", false, movable)) DoCut(lib_uid, rows);
    if (ImGui::MenuItem("Paste", "Ctrl+V", false, !g.clip.empty() && !g.changed)) DoPaste(lib, &row);
    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, movable)) DoDuplicate(rows);
    if (is_value) {
        ImGui::Separator();
        if (ImGui::MenuItem("Make default")) DoMakeDefault(rows);
        if (!node_uid.empty() && ImGui::MenuItem("Clear (plays the default)")) {
            Gesture("Clear value", [&](SoundStore& s) { return ClearValue(s, lib_uid, row); });
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Remove", "Del", false, movable)) DoRemove(rows);
    if (is_value && group_known) {
        if (ImGui::MenuItem("Remove value from group")) {
            const std::string gid = value_group, vid = row.key;
            Gesture("Remove value", [&](SoundStore& s) { return DeleteValue(s, gid, vid); });
        }
    }
    ImGui::EndPopup();
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
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, ui::kAccentSoft, ui::kRadiusSm);
    dl->AddRect(p0, p1, ui::kAccent, ui::kRadiusSm, 0, 1.5f);
    if (const ImGuiPayload* mp = ImGui::AcceptDragDropPayload(h.media_payload_type.c_str(),
                                                              ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
        const auto drop = h.media_drop;  // the hook may replace the hooks
        drop(lib_uid, row, mp->Data, mp->DataSize);
        g.changed = true;  // the hook may have rewritten the store
    }
}

// Rows dragged onto the empty space below the rows: into the root (a Switch root has no "into").
void EmptySpaceDropTarget(const SoundLibrary& lib)
{
    const float avail = ImGui::GetContentRegionAvail().y;
    const float h = std::max(ImGui::GetFrameHeight() * 2.0f, avail);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x), h));
    if (!ImGui::BeginDragDropTarget()) return;
    const ImVec2 p1 = ImGui::GetItemRectMax();
    const LibraryRow root_row{"", lib.root};
    const ImGuiPayload* pl = ImGui::GetDragDropPayload();
    if (pl && pl->IsDataType(kRowsPayload)) {
        DropTarget to;
        to.lib = lib.uid;
        to.row = root_row;
        to.mode = DropMode::Into;
        if (CanDrop(GetSoundStore(nullptr), g.drag_lib, g.drag_rows, to)) {
            ImGui::GetWindowDrawList()->AddRect(p0, p1, ui::kAccent, ui::kRadiusSm, 0, 1.5f);
            if (ImGui::AcceptDragDropPayload(kRowsPayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                const std::string src = g.drag_lib;
                const std::vector<LibraryRow> rows = g.drag_rows;
                const std::string lib_uid = lib.uid;
                std::vector<std::string> moved;
                if (Gesture("Move", [&](SoundStore& s) { return MoveRows(s, src, rows, to, &moved); }))
                    FocusNew(lib_uid, moved, false);
                g.drag_rows.clear();
            }
        }
    } else {
        MediaDropOn(lib.uid, root_row, p0, p1);
    }
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
            if (n->uid == lib.root) g.open.erase("!root");
            else g.open.insert(n->uid);
        }
    }

    const ImGuiPayload* pl = ImGui::GetDragDropPayload();
    if (pl && pl->IsDataType(kRowsPayload)) {
        const bool ok = CanDrop(GetSoundStore(nullptr), g.drag_lib, g.drag_rows, to);
        if (ok) {
            if (to.mode == DropMode::Before || to.mode == DropMode::After) {
                const float y = to.mode == DropMode::Before ? p0.y : p1.y;
                dl->AddLine(ImVec2(p0.x + r.depth * kIndent, y), ImVec2(p1.x, y), ui::kAccent, 2.0f);
            } else {
                dl->AddRect(p0, p1, ui::kAccent, ui::kRadiusSm, 0, 1.5f);
            }
            if (ImGui::AcceptDragDropPayload(kRowsPayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                const std::string src = g.drag_lib;
                const std::vector<LibraryRow> rows = g.drag_rows;
                const std::string lib_uid = lib.uid;  // `lib` is stale once the gesture rewrote the store
                std::vector<std::string> moved;
                if (Gesture("Move", [&](SoundStore& s) { return MoveRows(s, src, rows, to, &moved); })) {
                    if (const SoundLibrary* dl2 = FindLibrary(GetSoundStore(nullptr), lib_uid)) {
                        g.sel.clear();
                        for (const std::string& m : moved) {
                            RevealNode(*dl2, m);
                            g.sel.push_back(RowOfNode(*dl2, m));
                        }
                    }
                }
                g.drag_rows.clear();
            }
        } else {
            dl->AddRectFilled(p0, p1, ui::kHover);  // greyed: cannot drop here
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
    if (type == SoundNodeType::Switch) {
        if (MenuButton(label, ok > 0)) ImGui::OpenPopup("##tbswitch");
        Tip(why);
        if (ImGui::BeginPopup("##tbswitch")) {
            ImGui::TextDisabled("Switch on");
            bool ng = false;
            const std::string gid = GroupMenuItems(true, &ng);
            if (!gid.empty() || ng) DoAdd(SoundNodeType::Switch, gid, ng);
            ImGui::EndPopup();
        }
        return;
    }
    if (ok == 0) ImGui::BeginDisabled();
    if (ui::SolidButton(label)) DoAdd(type, {});
    if (ok == 0) ImGui::EndDisabled();
    Tip(why);
}

void HelpMark()
{
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Switch: plays one child, chosen by a Switch Group value (Material = Grass...).\n"
                          "Random: plays one of its children, picked at random at each event.\n"
                          "Blend: plays all its children together, layered.\n"
                          "Sound: one file or item (its Takes / take markers are its variations).\n\n"
                          "Shortcuts: F2 rename, Delete remove, Ctrl+D duplicate, Ctrl+X / Ctrl+V move,\n"
                          "Ctrl+Z undo, Space play. Ctrl / Shift-click selects several rows.");
}

void Toolbar(const SoundLibrary& lib)
{
    ToolbarAdd(lib, SoundNodeType::Sound, "+ Sound");
    if (g.changed) return;
    SameLineIfFits(ButtonWidth("+ Random"));
    ToolbarAdd(lib, SoundNodeType::Random, "+ Random");
    if (g.changed) return;
    SameLineIfFits(ButtonWidth("+ Blend"));
    ToolbarAdd(lib, SoundNodeType::Blend, "+ Blend");
    if (g.changed) return;
    SameLineIfFits(MenuButtonWidth("+ Switch"));
    ToolbarAdd(lib, SoundNodeType::Switch, "+ Switch");
    if (g.changed) return;
    if (g.sel.size() > 1) {
        const std::string cnt = "\xC2\xBB " + std::to_string(g.sel.size());
        SameLineIfFits(ImGui::CalcTextSize(cnt.c_str()).x);
        ImGui::TextColored(ui::Col(ui::kAccent), "%s", cnt.c_str());
        Tip("The + buttons add one to each selected row");
    }
    int with_node = 0;
    for (const LibraryRow& r : g.sel)
        if (!RowNode(lib, r).empty()) ++with_node;
    const std::string wl = with_node > 1 ? "Group (" + std::to_string(with_node) + ")" : std::string("Wrap");
    SameLineIfFits(MenuButtonWidth(wl.c_str()));
    if (MenuButton(wl.c_str(), with_node > 0)) ImGui::OpenPopup("##tbwrap");
    Tip(with_node > 1 ? "Groups the selected rows into a new Random or Blend"
                      : with_node ? "Wraps the selected row in a new Random or Blend"
                                  : "Select rows first (Ctrl / Shift-click for several)");
    if (ImGui::BeginPopup("##tbwrap")) {
        std::vector<LibraryRow> rows;
        for (const LibraryRow& r : g.sel)
            if (!RowNode(lib, r).empty()) rows.push_back(r);
        WrapMenu(rows);
        ImGui::EndPopup();
    }
    SameLineIfFits(ImGui::CalcTextSize("(?)").x);
    HelpMark();
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
    DrawRowBody(lib, r);
    ImGui::PopID();
    ImGui::PopID();
}

// One row; returns at once after a gesture (`lib` is then stale).
void DrawRowBody(const SoundLibrary& lib, const TreeRow& r)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rh = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float x0 = p.x + r.depth * kIndent;
    const float ty = p.y + (rh - ImGui::GetTextLineHeight()) * 0.5f;

    if (r.kind == TreeRowKind::UnassignedHeader) {
        dl->AddText(ImVec2(x0 + 14.0f, ty), kLibUnassigned, r.label.c_str());
        ImGui::Dummy(ImVec2(w, rh));
        return;
    }
    if (r.kind == TreeRowKind::AddValue) {
        ImGui::SetCursorScreenPos(ImVec2(x0 + 14.0f, p.y));
        std::string& text = g.add_value_text[r.row.parent];
        char buf[128] = {};
        std::snprintf(buf, sizeof(buf), "%s", text.c_str());
        ImGui::SetNextItemWidth(std::max(60.0f, std::min(160.0f, p.x + w - x0 - 20.0f)));
        const bool enter = ImGui::InputTextWithHint("##addval", "+ value", buf, sizeof(buf),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        text = buf;
        if (enter && !text.empty()) {
            const SoundNode* sw = FindNode(lib, r.row.parent);
            const std::string gid = sw ? sw->group_uid : std::string();
            const std::string name = text;
            if (Gesture("Add value", [&](SoundStore& s) { return AddValue(s, gid, name, nullptr); })) text.clear();
        }
        return;
    }

    const bool selected = IsSel(r.row);
    const bool is_cut = g.clip_lib == lib.uid && std::find(g.clip.begin(), g.clip.end(), r.row) != g.clip.end();
    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(w, rh));
    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    // Hover of the whole row (its overlaid buttons included), so they do not flicker.
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                         ImGui::IsMouseHoveringRect(r0, r1);
    if (r.kind != TreeRowKind::Hint) {
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
    }
    RowDropTarget(lib, r, r0, r1);
    if (g.changed) return;

    if (r.kind == TreeRowKind::Hint) {
        dl->AddText(ImVec2(x0 + 14.0f, ty), ui::kFaint, r.label.c_str());
        return;
    }

    const SoundNode* n = r.node.empty() ? nullptr : FindNode(lib, r.node);
    // Chevron.
    if (r.container) {
        ImGui::SetCursorScreenPos(ImVec2(x0, p.y));
        if (ImGui::InvisibleButton("##chev", ImVec2(12.0f, rh))) {
            if (r.node == lib.root) {
                if (r.open) g.open.insert("!root");
                else g.open.erase("!root");
            } else if (r.open) {
                g.open.erase(r.node);
            } else {
                g.open.insert(r.node);
            }
        }
        DrawChevron(dl, ImVec2(x0 + 6.0f, p.y + rh * 0.5f), r.open, ImGui::IsItemHovered() ? ui::kText : ui::kMuted);
    }
    // Type icon (a container's is a button: its type menu).
    const float is = rh - 8.0f;
    const ImVec2 ip(x0 + 14.0f, p.y + 4.0f);
    if (n) {
        bool hot = false;
        if (r.container) {
            ImGui::SetCursorScreenPos(ip);
            if (ImGui::InvisibleButton("##type", ImVec2(is, is))) ImGui::OpenPopup("##typemenu");
            hot = ImGui::IsItemHovered();
            if (hot) {
                std::string tip = TypeWord(n->type);
                if (n->type == SoundNodeType::Switch)
                    if (const SwitchGroup* gr = FindGroup(GetSoundStore(nullptr), n->group_uid)) tip += " on " + gr->name;
                ImGui::SetTooltip("%s \xC2\xB7 click to change the type", tip.c_str());
            }
        }
        DrawTypeIcon(dl, ip, is, n->type, hot);
    } else {
        dl->AddCircle(ImVec2(ip.x + is * 0.5f, ip.y + is * 0.5f), is * 0.3f, ui::kFaint);
    }
    if (ImGui::BeginPopup("##typemenu")) {
        TypeMenu(IsSel(r.row) ? g.sel : std::vector<LibraryRow>{r.row});
        ImGui::EndPopup();
    }
    if (g.changed) return;

    // Right side, from the right edge: ⋯, ▶ (hover / selected, play hook set), default / ☆, missing.
    float rx = p.x + w - 4.0f;
    const bool show_tools = hovered || selected || ImGui::IsPopupOpen("##typemenu");
    auto right_button = [&](const char* id, const char* text, ImU32 col) {
        const float bw = ImGui::CalcTextSize(text).x + 8.0f;
        rx -= bw;
        ImGui::SetCursorScreenPos(ImVec2(rx, p.y));
        const bool pressed = ImGui::InvisibleButton(id, ImVec2(bw, rh));
        dl->AddText(ImVec2(rx + 4.0f, ty), ImGui::IsItemHovered() ? ui::kText : col, text);
        rx -= 2.0f;
        return pressed;
    };
    if (show_tools && right_button("##more", "...", ui::kMuted)) {
        if (!selected) SelectOnly(r.row);
        g.menu_row = r.row;
        g.menu_rows = IsSel(r.row) ? g.sel : std::vector<LibraryRow>{r.row};
        g.open_row_menu = true;
    }
    if (show_tools && n && g.hooks.play) {
        const bool lit = g.hooks.is_lit && g.hooks.is_lit(n->uid);
        const float bw = 16.0f;
        rx -= bw;
        ImGui::SetCursorScreenPos(ImVec2(rx, p.y));
        if (ImGui::InvisibleButton("##play", ImVec2(bw, rh))) {
            if (lit && g.hooks.stop) g.hooks.stop();
            else g.hooks.play(lib.uid, n->uid);
        }
        const ImU32 pc = ImGui::IsItemHovered() || lit ? ui::kAccent : ui::kMuted;
        const float cy = p.y + rh * 0.5f;
        if (lit) dl->AddRectFilled(ImVec2(rx + 4.0f, cy - 4.0f), ImVec2(rx + 12.0f, cy + 4.0f), pc);
        else dl->AddTriangleFilled(ImVec2(rx + 4.0f, cy - 5.0f), ImVec2(rx + 4.0f, cy + 5.0f), ImVec2(rx + 13.0f, cy), pc);
        rx -= 2.0f;
    }
    if (r.kind == TreeRowKind::Value) {
        if (r.is_default) {
            const char* t = "default";
            const float tw = ImGui::CalcTextSize(t).x;
            rx -= tw + 4.0f;
            dl->AddText(ImVec2(rx, ty), ui::kMuted, t);
            ImGui::SetCursorScreenPos(ImVec2(rx, p.y));
            ImGui::Dummy(ImVec2(tw, rh));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Default value: plays when the event's value is not set, or when a value has no sound of its own");
        } else if (hovered) {
            if (right_button("##star", "*", ui::kMuted)) DoMakeDefault({r.row});
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Make it the default value");
        }
    }
    if (g.changed) return;
    const bool missing = n && SoundMissing(*n);
    if (missing) {
        const char* t = "missing";
        rx -= ImGui::CalcTextSize(t).x + 6.0f;
        dl->AddText(ImVec2(rx, ty), kLibMissing, t);
    }

    // The name (or its inline rename field).
    const float nx = ip.x + is + 6.0f;
    if (g.rename == RenameKind::Row && g.r_a == lib.uid && g.r_row == r.row) {
        ImGui::SetCursorScreenPos(ImVec2(nx, p.y));
        const int res = InlineName(rx - nx - 4.0f);
        if (res != 0) {
            const std::string name = g.buf;
            const LibraryRow row = r.row;
            const std::string lib_uid = lib.uid;
            EndRename();
            if (res > 0) Gesture("Rename", [&](SoundStore& s) { return RenameRow(s, lib_uid, row, name); });
        }
    } else {
        ImU32 col = ui::kText;
        if (r.kind == TreeRowKind::Value && r.node.empty()) col = ui::kFaint;  // an empty value, dimmed
        if (missing && r.kind == TreeRowKind::Node) col = kLibMissing;
        if (is_cut) col = ui::kFaint;
        if (g.hooks.is_lit && n && g.hooks.is_lit(n->uid)) col = ui::kAccent;
        ui::EllipsisText(dl, ImVec2(nx, ty), rx - 4.0f, col, r.label);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rh));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

// The keys of decision 7, while the Library view has the focus and no field takes them.
void TreeKeys(const SoundLibrary& lib)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (!LibraryViewFocused() || !g.tree_focused || io.WantTextInput || g.rename != RenameKind::None) return;
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
    const LibraryRow* first = g.sel.empty() ? nullptr : &g.sel.front();
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !g.sel.empty()) DoRemove(g.sel);
    else if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && first) StartRowRename(lib, *first);
    else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && !g.sel.empty()) DoDuplicate(g.sel);
    else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X, false) && !g.sel.empty()) {
        DoCut(lib.uid, g.sel);
    } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        DoPaste(lib, first);
    } else if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false) && first) {
        const bool lit = g.hooks.is_lit && g.hooks.is_lit(RowNode(lib, *first));
        if (lit && g.hooks.stop) g.hooks.stop();
        else PlayRow(lib, *first);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (g.hooks.stop) g.hooks.stop();
        g.clip.clear();
    }
}

void EmptyLibraryHint(const SoundLibrary& lib)
{
    ImGui::Spacing();
    const char* t = "This library is empty. Add a first sound:";
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (w - ImGui::CalcTextSize(t).x) * 0.5f));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", t);
    ImGui::PopTextWrapPos();
    const float bw = 4.0f * 80.0f + 3.0f * ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (w - bw) * 0.5f));
    (void)lib;
    if (ui::SolidButton("+ Sound##hint", ImVec2(80.0f, 0.0f))) DoAdd(SoundNodeType::Sound, {});
    SameLineIfFits(80.0f);
    if (ui::SolidButton("+ Random##hint", ImVec2(80.0f, 0.0f))) DoAdd(SoundNodeType::Random, {});
    SameLineIfFits(80.0f);
    if (ui::SolidButton("+ Blend##hint", ImVec2(80.0f, 0.0f))) DoAdd(SoundNodeType::Blend, {});
    SameLineIfFits(80.0f);
    if (ui::SolidButton("+ Switch##hint", ImVec2(80.0f, 0.0f))) ImGui::OpenPopup("##hintswitch");
    if (ImGui::BeginPopup("##hintswitch")) {
        bool ng = false;
        const std::string gid = GroupMenuItems(true, &ng);
        if (!gid.empty() || ng) DoAdd(SoundNodeType::Switch, gid, ng);
        ImGui::EndPopup();
    }
}

}  // namespace

// ---- Column 1 -----------------------------------------------------------------------------------

void DrawLibrariesSection()
{
    g.changed = false;
    RefreshItemGuids();
    const SoundStore& store = GetSoundStore(nullptr);
    CurrentLib();
    ui::Section sec;
    if (ui::BeginSection(sec, "Libraries")) {
        std::string to_delete;
        for (const SoundLibrary& lib : store.libraries) {
            ImGui::PushID(lib.uid.c_str());
            const float rh = ImGui::GetFrameHeight();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float ty = p.y + (rh - ImGui::GetTextLineHeight()) * 0.5f;
            if (g.rename == RenameKind::Library && g.r_a == lib.uid) {
                const int res = InlineName(w);
                if (res != 0) {
                    const std::string name = g.buf, uid = lib.uid;
                    EndRename();
                    if (res > 0) Gesture("Rename library", [&](SoundStore& s) { return RenameLibrary(s, uid, name); });
                }
                ImGui::PopID();
                if (g.changed) break;
                continue;
            }
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::Selectable("##lib", g.lib == lib.uid, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(w, rh))) {
                if (g.lib != lib.uid) {
                    g.lib = lib.uid;
                    g.sel.clear();
                    g.has_anchor = false;
                }
            }
            const bool hovered = ImGui::IsItemHovered();
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                g.rename = RenameKind::Library;
                g.r_a = lib.uid;
                std::snprintf(g.buf, sizeof(g.buf), "%s", lib.name.c_str());
                g.r_focus = true;
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##libmenu");
            ImU32 dot = ui::kAccent;  // colour 0 = the accent
            if (lib.color & 0x1000000u)
                dot = IM_COL32((lib.color >> 16) & 0xFF, (lib.color >> 8) & 0xFF, lib.color & 0xFF, 0xFF);
            dl->AddCircleFilled(ImVec2(p.x + 6.0f, p.y + rh * 0.5f), 4.0f, dot);
            // Right side: ⋯, "!", missing count, "User".
            float rx = p.x + w - 4.0f;
            if (hovered || g.lib == lib.uid) {
                rx -= 22.0f;
                ImGui::SetCursorScreenPos(ImVec2(rx, p.y));
                if (ImGui::InvisibleButton("##more", ImVec2(22.0f, rh))) ImGui::OpenPopup("##libmenu");
                dl->AddText(ImVec2(rx + 4.0f, ty), ImGui::IsItemHovered() ? ui::kText : ui::kMuted, "...");
            }
            const LibraryCheck chk = CheckLibrary(store, lib);
            if (chk.blocked) {
                rx -= 14.0f;
                ImGui::SetCursorScreenPos(ImVec2(rx, p.y));
                ImGui::Dummy(ImVec2(12.0f, rh));
                dl->AddText(ImVec2(rx + 3.0f, ty), ui::kWarn, "!");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Blocked: %s", chk.reason.c_str());
            }
            const int miss = CountMissing(lib, SoundMissing);
            if (miss > 0) {
                const std::string t = std::to_string(miss);
                rx -= ImGui::CalcTextSize(t.c_str()).x + 6.0f;
                ImGui::SetCursorScreenPos(ImVec2(rx, p.y));
                ImGui::Dummy(ImVec2(ImGui::CalcTextSize(t.c_str()).x, rh));
                dl->AddText(ImVec2(rx, ty), kLibMissing, t.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%d missing sound(s)", miss);
            }
            if (lib.user) {
                rx -= ImGui::CalcTextSize("User").x + 6.0f;
                dl->AddText(ImVec2(rx, ty), ui::kMuted, "User");
            }
            ui::EllipsisText(dl, ImVec2(p.x + 16.0f, ty), rx - 4.0f, ui::kText, lib.name);
            if (ImGui::BeginPopup("##libmenu")) {
                if (ImGui::MenuItem("Rename")) {
                    g.rename = RenameKind::Library;
                    g.r_a = lib.uid;
                    std::snprintf(g.buf, sizeof(g.buf), "%s", lib.name.c_str());
                    g.r_focus = true;
                }
                if (ImGui::MenuItem("Delete")) to_delete = lib.uid;
                ImGui::EndPopup();
            }
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rh));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            ImGui::PopID();
        }
        if (!to_delete.empty())
            Gesture("Delete library", [&](SoundStore& s) { return DeleteLibrary(s, to_delete); });
        if (store.libraries.empty()) ImGui::TextDisabled("No library yet");
        if (ui::SolidButton("+ Library")) {
            std::string uid;
            if (Gesture("Add library", [&](SoundStore& s) { return NewLibrary(s, &uid); })) {
                g.lib = uid;
                g.sel.clear();
                g.has_anchor = false;
            }
        }
    }
    ui::EndSection(sec);
}

void DrawSwitchGroupsSection()
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
    ui::Section sec;
    if (ui::BeginSection(sec, "Switch Groups")) {
        std::string del_group, del_value_group, del_value;
        for (const SwitchGroup& gr : store.groups) {
            ImGui::PushID(gr.uid.c_str());
            const float w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
            bool open = false;
            if (g.rename == RenameKind::Group && g.r_a == gr.uid) {
                const int res = InlineName(w);
                if (res != 0) {
                    const std::string name = g.buf, uid = gr.uid;
                    EndRename();
                    if (res > 0) Gesture("Rename group", [&](SoundStore& s) { return RenameGroup(s, uid, name); });
                }
                if (g.changed) {
                    ImGui::PopID();
                    break;
                }
                open = ImGui::GetStateStorage()->GetBool(ImGui::GetID("##grp"), true);
            } else {
                const ImVec2 hp = ImGui::GetCursorScreenPos();
                const float bw = ButtonWidth("+") + ImGui::GetStyle().ItemSpacing.x + ButtonWidth("\xC3\x97");
                ImGui::SetNextItemAllowOverlap();
                ImGui::SetNextItemOpen(ImGui::GetStateStorage()->GetBool(ImGui::GetID("##grp"), true), ImGuiCond_Once);
                open = ImGui::TreeNodeEx("##grp", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen |
                                                      ImGuiTreeNodeFlags_NoTreePushOnOpen,
                                         "%s", "");
                const float gty = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y - ImGui::GetTextLineHeight()) * 0.5f;
                ui::EllipsisText(ImGui::GetWindowDrawList(), ImVec2(hp.x + ImGui::GetTreeNodeToLabelSpacing(), gty),
                                 hp.x + w - bw - 6.0f, ui::kText, gr.name);
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    g.rename = RenameKind::Group;
                    g.r_a = gr.uid;
                    std::snprintf(g.buf, sizeof(g.buf), "%s", gr.name.c_str());
                    g.r_focus = true;
                }
                ImGui::SameLine();
                ImGui::SetCursorScreenPos(ImVec2(std::max(hp.x, hp.x + w - bw), hp.y));
                if (ImGui::SmallButton("+")) {
                    std::string vid;
                    const std::string gid = gr.uid;
                    if (Gesture("Add value", [&](SoundStore& s) { return AddValue(s, gid, "Value", &vid); })) {
                        ImGui::GetStateStorage()->SetBool(ImGui::GetID("##grp"), true);  // its field must be drawn
                        g.rename = RenameKind::Value;
                        g.r_a = gid;
                        g.r_b = vid;
                        if (const SwitchGroup* ng = FindGroup(GetSoundStore(nullptr), gid))
                            for (const SwitchValue& v : ng->values)
                                if (v.uid == vid) std::snprintf(g.buf, sizeof(g.buf), "%s", v.name.c_str());
                        g.r_focus = true;
                    }
                }
                Tip("Add a value");
                ImGui::SameLine();
                if (ImGui::SmallButton("\xC3\x97")) del_group = gr.uid;
                Tip("Delete the group: every Switch on it becomes a Random, its sounds kept");
                if (g.changed) {
                    ImGui::PopID();
                    break;
                }
            }
            if (open) {
                ImGui::Indent(kIndent + 4.0f);
                for (const SwitchValue& v : gr.values) {
                    ImGui::PushID(v.uid.c_str());
                    const float vw = std::max(1.0f, ImGui::GetContentRegionAvail().x);
                    if (g.rename == RenameKind::Value && g.r_a == gr.uid && g.r_b == v.uid) {
                        const int res = InlineName(vw);
                        if (res != 0) {
                            const std::string name = g.buf, gid = gr.uid, vid = v.uid;
                            EndRename();
                            if (res > 0)
                                Gesture("Rename value", [&](SoundStore& s) { return RenameValue(s, gid, vid, name); });
                        }
                        if (g.changed) {
                            ImGui::PopID();
                            break;
                        }
                    } else {
                        const ImVec2 vp = ImGui::GetCursorScreenPos();
                        const float xw = ButtonWidth("\xC3\x97");
                        ImGui::SetNextItemAllowOverlap();
                        ImGui::Selectable("##val", false, ImGuiSelectableFlags_AllowDoubleClick,
                                          ImVec2(std::max(1.0f, vw - xw - 4.0f), 0.0f));
                        ui::EllipsisText(ImGui::GetWindowDrawList(), vp, vp.x + vw - xw - 6.0f, ui::kText, v.name);
                        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            g.rename = RenameKind::Value;
                            g.r_a = gr.uid;
                            g.r_b = v.uid;
                            std::snprintf(g.buf, sizeof(g.buf), "%s", v.name.c_str());
                            g.r_focus = true;
                        }
                        ImGui::SameLine();
                        ImGui::SetCursorScreenPos(ImVec2(std::max(vp.x, vp.x + vw - xw), vp.y));
                        if (ImGui::SmallButton("\xC3\x97")) {
                            del_value_group = gr.uid;
                            del_value = v.uid;
                        }
                        Tip("Remove the value: its sounds go to Unassigned in every Switch on this group");
                    }
                    ImGui::PopID();
                }
                if (!g.changed && gr.values.empty()) ImGui::TextDisabled("No value: press +");
                ImGui::Unindent(kIndent + 4.0f);
            }
            ImGui::PopID();
            if (g.changed) break;
        }
        if (!del_group.empty()) Gesture("Delete group", [&](SoundStore& s) { return DeleteGroup(s, del_group); });
        if (!del_value.empty())
            Gesture("Remove value", [&](SoundStore& s) { return DeleteValue(s, del_value_group, del_value); });
        if (ui::SolidButton("+ Group")) {
            std::string gid;
            if (Gesture("Add group", [&](SoundStore& s) { return AddGroup(s, "Group", &gid); })) {
                g.rename = RenameKind::Group;
                g.r_a = gid;
                std::snprintf(g.buf, sizeof(g.buf), "%s",
                              FindGroup(GetSoundStore(nullptr), gid) ? FindGroup(GetSoundStore(nullptr), gid)->name.c_str() : "Group");
                g.r_focus = true;
            }
        }
    }
    ui::EndSection(sec);
}

// ---- Column 2 -----------------------------------------------------------------------------------

void DrawLibraryTree()
{
    g.changed = false;
    g.tree_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);  // column 2 and its tree
    RefreshItemGuids();
    const SoundStore& store = GetSoundStore(nullptr);
    const SoundLibrary* lib = CurrentLib();
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    ImGui::Indent(6.0f);
    if (!lib) {
        ImGui::TextDisabled("No library yet");
        if (ui::SolidButton("+ Library##tree")) {
            std::string uid;
            if (Gesture("Add library", [&](SoundStore& s) { return NewLibrary(s, &uid); })) g.lib = uid;
        }
        ImGui::Unindent(6.0f);
        return;
    }
    // Header: the library's name (a switcher while column 1 is collapsed).
    if (LibraryColumn1Collapsed()) {
        if (MenuButton(lib->name.c_str())) ImGui::OpenPopup("##libswitch");
        if (ImGui::BeginPopup("##libswitch")) {
            for (const SoundLibrary& l : store.libraries) {
                ImGui::PushID(l.uid.c_str());
                if (ImGui::MenuItem(l.name.c_str(), nullptr, l.uid == g.lib)) {
                    g.lib = l.uid;
                    g.sel.clear();
                    g.has_anchor = false;
                }
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
    } else {
        const ImVec2 np = ImGui::GetCursorScreenPos();
        const float nw = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        ImGui::Dummy(ImVec2(nw, ImGui::GetTextLineHeight()));
        ui::EllipsisText(ImGui::GetWindowDrawList(), np, np.x + nw, ui::kText, lib->name);
        if (ImGui::IsItemHovered() && ImGui::CalcTextSize(lib->name.c_str()).x > nw) ImGui::SetTooltip("%s", lib->name.c_str());
    }
    lib = CurrentLib();
    if (!lib) {
        ImGui::Unindent(6.0f);
        return;
    }
    Toolbar(*lib);
    ImGui::Unindent(6.0f);
    ImGui::Separator();
    if (!g.changed) TreeKeys(*lib);  // after a toolbar gesture `lib` is stale
    lib = CurrentLib();  // a gesture above rewrote the store
    if (!lib) return;

    g.rows = BuildTreeRows(GetSoundStore(nullptr), *lib, g.open);
    // The selection is pruned against the rows (an undo may remove uids).
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
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 1.0f));
        const std::vector<TreeRow> rows = g.rows;  // a gesture may rebuild g.rows
        for (size_t i = 0; i < rows.size() && !g.changed; ++i) {
            const SoundLibrary* cur = FindLibrary(GetSoundStore(nullptr), g.lib);
            if (!cur) break;
            DrawRow(*cur, rows[i], i);
        }
        ImGui::PopStyleVar();
        lib = FindLibrary(GetSoundStore(nullptr), g.lib);
        const SoundNode* root = lib ? FindNode(*lib, lib->root) : nullptr;
        if (!g.changed && root && root->type != SoundNodeType::Switch && ChildrenOf(*lib, root->uid).empty())
            EmptyLibraryHint(*lib);
        lib = FindLibrary(GetSoundStore(nullptr), g.lib);
        if (!g.changed && lib && FindNode(*lib, lib->root)) EmptySpaceDropTarget(*lib);
        if (!ImGui::GetDragDropPayload()) g.hover_uid.clear();
        if (g.open_row_menu) {
            ImGui::OpenPopup("##rowmenu");
            g.open_row_menu = false;
        }
        if (const SoundLibrary* cur = FindLibrary(GetSoundStore(nullptr), g.lib)) RowMenu(*cur);
    }
    ImGui::EndChild();
}

// ---- Column 3: several rows selected ------------------------------------------------------------

void DrawLibraryBatchPanel()
{
    g.changed = false;
    const SoundStore& store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, g.lib);
    if (!lib) return;
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    ImGui::Indent(6.0f);
    ImGui::Text("%d rows selected", static_cast<int>(g.sel.size()));
    ImGui::Unindent(6.0f);
    const std::vector<LibraryRow> rows = g.sel;
    // The common type of the selected containers (-1 = mixed or none).
    int common = -2;
    std::string common_group;
    int values = 0;
    for (const LibraryRow& r : rows) {
        if (RowIsValue(*lib, r)) ++values;
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
    ui::Section sec;
    if (ui::BeginSection(sec, "Batch edit", ImTextureID(), false)) {
        static const char* const kTypes[3] = {"Random", "Blend", "Switch"};
        int sel = common >= 0 ? common : -1;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Type");
        float seg_w = 2.0f;
        for (const char* t : kTypes) seg_w += ImGui::CalcTextSize(t).x + 26.0f;
        SameLineIfFits(seg_w);
        const int was = sel;
        if (ui::Segmented("##batchtype", kTypes, 3, &sel)) {
            if (sel == 0) DoSetType(rows, SoundNodeType::Random, {});
            else if (sel == 1) DoSetType(rows, SoundNodeType::Blend, {});
            else ImGui::OpenPopup("##batchswitch");
        } else if (was == 2 && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && ImGui::IsItemHovered() &&
                   !ImGui::IsPopupOpen("##batchswitch") &&
                   ImGui::IsMouseHoveringRect(ImVec2(ImGui::GetItemRectMax().x - (ImGui::CalcTextSize(kTypes[2]).x + 26.0f),
                                                     ImGui::GetItemRectMin().y),
                                              ImGui::GetItemRectMax())) {
            // Bug 3: all already Switch, the segment does not "change": it still picks the group.
            ImGui::OpenPopup("##batchswitch");
        }
        if (common == -1) {
            SameLineIfFits(ImGui::CalcTextSize("mixed").x);
            ImGui::TextDisabled("mixed");
        }
        if (ImGui::BeginPopup("##batchswitch")) {
            const std::string gid = GroupMenuItems(false, nullptr);
            if (!gid.empty()) BatchSwitchOn(rows, gid);
            ImGui::EndPopup();
        }
        const SwitchGroup* cg = common == 2 ? FindGroup(store, common_group) : nullptr;
        const std::string sw_label = std::string("Switch on: ") + (cg ? cg->name : std::string("..."));
        if (MenuButton(sw_label.c_str())) ImGui::OpenPopup("##batchswitch2");
        Tip("Also converts the Random and Blend containers");
        if (ImGui::BeginPopup("##batchswitch2")) {
            const std::string gid = GroupMenuItems(false, nullptr);
            if (!gid.empty()) BatchSwitchOn(rows, gid);
            ImGui::EndPopup();
        }
        SameLineIfFits(MenuButtonWidth("Put into"));
        if (MenuButton("Put into")) ImGui::OpenPopup("##putinto");
        if (ImGui::BeginPopup("##putinto")) {
            if (ImGui::MenuItem("New Random")) DoWrap(rows, SoundNodeType::Random, {});
            if (ImGui::MenuItem("New Blend")) DoWrap(rows, SoundNodeType::Blend, {});
            if (ImGui::BeginMenu("New Switch on")) {
                const std::string gid = GroupMenuItems(false, nullptr);
                if (!gid.empty()) DoWrap(rows, SoundNodeType::Switch, gid);
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
                if (ImGui::MenuItem(n.name.c_str(), TypeWord(n.type), false, ok)) {
                    const std::string lib_uid = lib->uid;
                    Gesture("Put into " + n.name, [&](SoundStore& s) { return MoveRows(s, lib_uid, rows, to, nullptr); });
                }
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
        if (values == 0) ImGui::BeginDisabled();
        SameLineIfFits(ButtonWidth("Make them default"));
        if (ui::SolidButton("Make them default")) DoMakeDefault(rows);
        if (values == 0) ImGui::EndDisabled();
        Tip(values ? "One default per Switch" : "Select Switch value rows first");
    }
    ui::EndSection(sec);
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
const LibraryTreeHooks& GetLibraryTreeHooks() { return g.hooks; }

}  // namespace rav

#endif  // _WIN32
