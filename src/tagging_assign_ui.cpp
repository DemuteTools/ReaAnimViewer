// SPDX-License-Identifier: MIT
//
// See tagging_assign_ui.h. The decisions (targets, summaries, mixed selections, selection rules, the
// Plays / ladder texts) are assign_view_model.h's; this file draws them with the house theme (ui_theme.h,
// ui_flow.h) and queues every gesture through the picked-gestures mechanism (the window lists the sound
// store by reference, and a gesture replaces it).

#include "tagging_assign_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>  // ClosePopupToLevel (Esc closes an open menu)

#include "assign_view_model.h"
#include "float_ui_window.h"
#include "item_rules.h"
#include "library_icons.h"
#include "library_tree_ui.h"
#include "reaper_api.h"
#include "shortcuts.h"  // LoadPrefFloat / SavePrefFloat
#include "sound_assign.h"
#include "sound_auto_assign.h"
#include "sound_populate.h"
#include "sound_project.h"
#include "sound_record.h"
#include "tagging_populate_ui.h"
#include "tagging_session.h"
#include "ui_flow.h"
#include "ui_theme.h"
#include "video_view.h"

namespace rav {

namespace {

namespace av = assign;

const char kDot[] = " \xC2\xB7 ";

// Antho 2026-10-12: a window of its own, anywhere on the screen (FloatUiWindow); its rect is kept in screen
// pixels under "assign.float.*". First-use and minimum sizes in dp (the mockup's).
constexpr int kDefaultW = 880, kDefaultH = 600;
constexpr int kMinW = 460, kMinH = 340;
FloatUiWindow g_host("assign.float", L"Sound assignment", kDefaultW, kDefaultH, kMinW, kMinH);

enum Scope { kScopeProject = 0, kScopeCharacter = 1, kScopeItem = 2 };

// Seconds on a process clock (review: not ImGui's time, which a new context of the window starts again at 0).
double ClockSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The model signature of a rig (ItemRigInfo::model): its meshes, so the clips of one model are one character.
std::string RigModel(const ItemRigInfo& rig)
{
    return rig.model;
}

struct State {
    bool       open = false;
    MediaItem* item = nullptr;  // the window's item (follows the Tagging view's unless the user picked one)
    MediaItem* last_model_item = nullptr;
    MediaItem* sel_item = nullptr;  // the item the selection and the folds belong to
    bool       follow = true;
    int        scope = kScopeItem;
    bool       overview = false;
    av::Selection sel;
    std::string   sel_sig;
    std::map<std::string, bool> closed;  // parent row id -> folded (rows start open)
    std::map<std::string, bool> folded;  // section id ("item", "rules", "auto") -> folded
    char        filter[96] = {};
    float       split = -1.0f;  // the tree's width, px
    std::string nav;            // the row the keyboard is on ("item", a leaf key, "P:<parent id>")
    bool        nav_shown = false;
    bool        nav_scroll = false;
    bool        has_last = false;
    av::ChangeText last;
    std::string last_note;      // a view-only note ("Expanded every row ...")
    // A request from the strip.
    bool             request = false;
    MediaItem*       request_item = nullptr;
    std::vector<int> request_blocks;
    bool             focus_request = false;
    // 13-15c: the Project and Character scopes.
    av::Selection sel_by[3];  // each scope's selection while another is shown
    bool          key_init[3] = {false, false, false};
    std::string   cur_skel, cur_model;  // the current character: its rig and model signature
    char          name_buf[96] = {};    // the character sheet's Name field
    std::string   name_for;             // the character (its row key) name_buf holds the name of
    bool          name_active = false;
    CharacterRef  name_ref;             // review 13-15c: the character being renamed, its project, its name before
    ReaProject*   name_proj = nullptr;
    std::string   name_orig;
    ImVec2        name_min, name_max;   // the field's rect (a click outside it ends the typing)
    int           name_frame = -1;      // the frame the field was last drawn
    int           rigs_count = -1;      // the project state, and the time, the rig list was read at
    double        rigs_time = 0.0;
    std::vector<av::RigIn> rigs;
    std::string   kit_skel, kit_model;  // the kit sheet's character picker (its rig: a key changes when stored)
};
State g;

// ---- Per-frame data ---------------------------------------------------------------------------------

// What the Project and Character scopes read (13-15c): the project's characters and, per body part and type, the
// target of that key at this scope.
struct KeyData {
    std::vector<av::CharEntry>        chars;
    int                               cur = -1;  // the current character (its index), -1 when the project has none
    const SoundCharacter*             scope_ch = nullptr;  // Character scope: the current character's record, else null
    std::map<std::string, av::Target> targets;   // by key ("feet", "feet.step"); empty in the Overview
    std::set<std::string>             own;       // the keys this scope sets (a library or a Switch value)
};

struct Frame {
    bool                   keyed = false;  // Project / Character scope: `key` is read, not the item
    KeyData                key;
    const SoundStore*      store = nullptr;
    ItemSounds             rec;
    const SoundCharacter*  ch = nullptr;
    std::vector<Block>     blocks;
    av::Targets            tg;
    std::vector<av::Target> targets;  // by block index
    av::Target             item;      // the "Item values" row
    std::string            item_name, char_name, pool_text;
    MediaItem*             item_ptr = nullptr;  // the item this frame was built from
    bool                   ok = false;
};

struct RecCache {
    MediaItem*         item = nullptr;
    int                count = -1;
    ItemSounds         rec;
    std::vector<Block> blocks;
    bool               valid = false;
    int                pool = 0;
    ItemRigInfo        rig;
    double             rig_time = 0.0;  // when the rig was last read (ClockSeconds)
};
// Review 13-15b: the window and the strip each keep their item's, so neither reads it again every frame
// when they show different items.
RecCache g_cache, g_strip_cache;

std::string ItemNameOf(MediaItem* item)
{
    if (!item) return std::string();
    if (MediaItem_Take* take = RavTakeOf(item))
        if (const char* nm = GetTakeName(take)) return nm;
    return std::string();
}

std::string LeafTitle(const av::Parent& p, const av::Leaf& l)
{
    return p.leaves.size() > 1 ? p.name + kDot + l.name : l.name;
}

// The item's sound record, rules, pool size and rig, read again when the project's state changes.
void RefreshCache(MediaItem* item, RecCache& c = g_cache)
{
    const int count = GetProjectStateChangeCount(nullptr);
    if (item == c.item && count == c.count) {
        // Review: a re-exported file changes no project state: its rig is checked again each second.
        const double now = ClockSeconds();
        if (now - c.rig_time >= 1.0) {
            c.rig = ItemRig(item);
            c.rig_time = now;
        }
        return;
    }
    c.rig_time = ClockSeconds();
    c.item = item;
    c.count = count;
    c.rec = ReadItemSounds(item);
    ItemRulesRead rr;
    c.valid = ReadItemRules(item, &rr) && rr.valid;
    c.blocks = c.valid ? rr.rules.blocks : std::vector<Block>();
    c.pool = std::max(0, static_cast<int>(PopulateTargets({item}).size()) - 1);
    c.rig = ItemRig(item);
}

bool BuildFrame(MediaItem* item, Frame* f)
{
    if (!item || !ValidatePtr2(nullptr, item, "MediaItem*") || !RavTakeOf(item)) return false;
    f->store = &GetSoundStore(nullptr);
    RefreshCache(item);
    f->rec = g_cache.rec;
    f->blocks = g_cache.blocks;
    f->item_ptr = item;
    const ItemRigInfo& rig = g_cache.rig;
    f->ch = CharacterFor(*f->store, rig.skeleton, RigModel(rig));
    f->char_name = f->ch ? f->ch->name : std::string();
    f->item_name = ItemNameOf(item);
    f->pool_text = g_cache.pool > 0 ? "this item + " + std::to_string(g_cache.pool) + " pool cop" + (g_cache.pool == 1 ? "y" : "ies")
                                    : std::string("this item");
    f->tg = av::BuildTargets(f->blocks);
    f->targets.resize(f->blocks.size());
    for (const std::vector<av::Parent>* list : {&f->tg.rules, &f->tg.autos})
        for (const av::Parent& p : *list)
            for (const av::Leaf& l : p.leaves)
                f->targets[static_cast<size_t>(l.block)] =
                    av::MakeTarget(*f->store, f->rec, f->ch, f->blocks[static_cast<size_t>(l.block)], l.key, LeafTitle(p, l));
    f->item = av::MakeItemTarget(*f->store, f->rec, f->targets);
    f->ok = true;
    return true;
}

// The rigs of the project's items, read again when the project changes (and every few seconds: a re-exported
// file changes no project state); ProjectRigs walks every RAV item, so never per frame.
void RefreshRigs()
{
    const int    count = GetProjectStateChangeCount(nullptr);
    const double now = ClockSeconds();
    if (count == g.rigs_count && now - g.rigs_time < 3.0) return;
    g.rigs_count = count;
    g.rigs_time = now;
    g.rigs.clear();
    for (const ProjectRig& r : ProjectRigs()) {
        av::RigIn in;
        in.skeleton = r.skeleton;
        in.bones = r.bones;
        in.items = r.items;
        for (const RigModelCount& m : r.models) in.models.push_back({m.model, m.files, m.items});
        g.rigs.push_back(in);
    }
}

// The current character's index: the one last picked, else the window's item's, else the first.
int FindCurrentChar(const std::vector<av::CharEntry>& cs)
{
    if (cs.empty()) return -1;
    for (size_t i = 0; i < cs.size(); ++i)
        if (cs[i].ch.skeleton == g.cur_skel && cs[i].ch.model == g.cur_model) return static_cast<int>(i);
    for (size_t i = 0; i < cs.size(); ++i)
        if (cs[i].ch.skeleton == g.cur_skel && cs[i].ch.model.empty()) return static_cast<int>(i);
    size_t pick = 0;
    if (g.item && ValidatePtr2(nullptr, g.item, "MediaItem*")) {
        const ItemRigInfo rig = ItemRig(g.item);
        for (size_t i = 0; i < cs.size(); ++i)
            if (cs[i].ch.skeleton == rig.skeleton && (cs[i].ch.model == RigModel(rig) || (cs[i].ch.model.empty() && cs[pick].ch.skeleton != rig.skeleton)))
                pick = i;
    }
    g.cur_skel = cs[pick].ch.skeleton;
    g.cur_model = cs[pick].ch.model;
    return static_cast<int>(pick);
}

// The Project / Character scopes' frame: no item needed.
void BuildKeyFrame(Frame* f)
{
    f->store = &GetSoundStore(nullptr);
    f->keyed = true;
    RefreshRigs();
    KeyData& k = f->key;
    k.chars = av::BuildCharacters(*f->store, g.rigs);
    k.cur = FindCurrentChar(k.chars);
    const bool char_scope = g.scope == kScopeCharacter;
    if (char_scope && k.cur >= 0) k.scope_ch = &k.chars[static_cast<size_t>(k.cur)].ch;
    f->ok = !char_scope || k.scope_ch != nullptr;
    f->item_name = char_scope ? (k.scope_ch ? "Character " + k.scope_ch->name : std::string("Character")) : std::string("Project defaults");
    f->char_name = k.scope_ch ? k.scope_ch->name : std::string();
    if (!f->ok) return;
    for (const std::string& key : av::OverrideKeys(k.scope_ch ? k.scope_ch->assigns : f->store->project)) k.own.insert(key);
    if (!g.overview)
        for (const std::string& key : av::AllKeys()) k.targets[key] = av::MakeKeyTarget(*f->store, k.scope_ch, key);
}

const av::Target* TargetOf(const Frame& f, const std::string& key)
{
    if (f.keyed) {
        const auto it = f.key.targets.find(key);
        return it == f.key.targets.end() ? nullptr : &it->second;
    }
    if (key == av::kItemKey) return &f.item;
    int b = -1;
    if (av::BlockOfKey(key, &b) && b >= 0 && static_cast<size_t>(b) < f.targets.size()) return &f.targets[static_cast<size_t>(b)];
    return nullptr;
}

std::vector<const av::Target*> SelectedTargets(const Frame& f)
{
    std::vector<const av::Target*> out;
    for (const std::string& k : g.sel.keys)
        if (const av::Target* t = TargetOf(f, k)) out.push_back(t);
    return out;
}

// ---- Summary painting -----------------------------------------------------------------------------------

ImU32 ToneInk(av::SegTone t)
{
    switch (t) {
    case av::SegTone::Normal: return ui::kText;
    case av::SegTone::Inherited: return ui::kMuted;
    case av::SegTone::Mixed:
    case av::SegTone::Warn: return ui::kWarn;
    case av::SegTone::Ok: return ui::kOk;
    case av::SegTone::Faint: break;
    }
    return ui::kFaint;
}

float SummaryNatWidth(const av::Summary& s)
{
    float w = s.has_dot ? ui::Dp(14.0f) : 0.0f;
    for (const av::Seg& g : s.segs) w += ImGui::CalcTextSize(g.text.c_str()).x;
    return w;
}

// Paints the summary in [x, x + max_w] on a line starting at y (right aligned when it fits and asked to);
// a summary too long ends with "...". True when cut.
bool PaintSummary(ImDrawList* dl, float x, float y, float max_w, const av::Summary& s, bool right_align)
{
    const float nat = SummaryNatWidth(s);
    float       x0 = x;
    if (right_align && nat < max_w) x0 = x + max_w - nat;
    const float xr = x + max_w;
    if (s.has_dot) {
        if (x0 + ui::Dp(8.0f) <= xr)
            dl->AddCircleFilled(ImVec2(x0 + ui::Dp(4.0f), y + ImGui::GetTextLineHeight() * 0.5f), ui::Dp(4.0f), ui::LibDot(s.dot_color));
        x0 += ui::Dp(14.0f);
    }
    for (const av::Seg& g : s.segs) {
        const float tw = ImGui::CalcTextSize(g.text.c_str()).x;
        if (x0 + tw <= xr) {
            dl->AddText(ImVec2(x0, y), ToneInk(g.tone), g.text.c_str());
            x0 += tw;
        } else {
            const std::string cut = ui::EllipsizeToWidth(ImGui::GetFont(), g.text, std::max(0.0f, xr - x0));
            dl->AddText(ImVec2(x0, y), ToneInk(g.tone), cut.c_str());
            return true;
        }
    }
    return false;
}

// ---- Gestures ---------------------------------------------------------------------------------------------

void SetLast(const std::string& undo, const std::string& where, const std::string& what, int n)
{
    g.last = av::LastChange(undo, where, what, n);
    g.has_last = true;
    g.last_note.clear();
}

// The rids of the selected leaf targets that have one, and the blocks of those that have none yet.
void RulesOf(const Frame& f, const std::vector<const av::Target*>& sel, std::vector<std::string>* rids, std::vector<int>* blocks)
{
    for (const av::Target* t : sel) {
        int b = -1;
        if (!av::BlockOfKey(t->key, &b) || b < 0 || static_cast<size_t>(b) >= f.blocks.size()) continue;
        const std::string& rid = f.blocks[static_cast<size_t>(b)].rid;
        if (rid.empty()) blocks->push_back(b);
        else rids->push_back(rid);
    }
}

// A Project / Character key's library (13-15c): one undo point for all the keys.
void PickKeyLibrary(const Frame& f, const std::vector<const av::Target*>& sel, const std::string& arg, const std::string& what)
{
    std::vector<std::string> keys;
    bool                     differs = false;
    for (const av::Target* t : sel) {
        keys.push_back(t->key);
        if (av::OwnLibraryArg(*t) != arg) differs = true;
    }
    if (!differs || (f.key.scope_ch && f.key.cur < 0)) return;
    LibState    state = LibState::Inherit;
    std::string uid;
    if (arg == kLibNoneWord) state = LibState::None;
    else if (!arg.empty()) {
        state = LibState::Library;
        uid = arg;
    }
    const bool         project = f.key.scope_ch == nullptr;
    const CharacterRef ref = project ? CharacterRef() : f.key.chars[static_cast<size_t>(f.key.cur)].ref;
    const std::string  where = project ? std::string("Project") : f.key.scope_ch->name;
    const int          n = static_cast<int>(sel.size());
    PickSoundGesture([project, ref, keys, state, uid, what, where, n]() {
        const int         c0 = GetProjectStateChangeCount(nullptr);
        const SoundReport rep = project ? SetProjectAssignLibrary(keys, state, uid) : SetCharacterAssignLibrary(ref, keys, state, uid);
        if (GetProjectStateChangeCount(nullptr) != c0)
            SetLast(project ? "Set project sound default" : "Set character library", where, what, n);
        return rep;
    });
}

void PickKeyValue(const Frame& f, const std::vector<const av::Target*>& sel, const std::string& group, const std::string& value,
                  const std::string& what)
{
    std::vector<std::string> keys;
    bool                     differs = false;
    for (const av::Target* t : sel) {
        keys.push_back(t->key);
        const av::GroupState* gs = t->Group(group);
        if (gs && gs->own_uid != value) differs = true;
    }
    if (!differs || (f.key.scope_ch && f.key.cur < 0)) return;
    const bool         project = f.key.scope_ch == nullptr;
    const CharacterRef ref = project ? CharacterRef() : f.key.chars[static_cast<size_t>(f.key.cur)].ref;
    const std::string  where = project ? std::string("Project") : f.key.scope_ch->name;
    const int          n = static_cast<int>(sel.size());
    PickSoundGesture([project, ref, keys, group, value, what, where, n]() {
        const int         c0 = GetProjectStateChangeCount(nullptr);
        const SoundReport rep = project ? SetProjectAssignValue(keys, group, value) : SetCharacterAssignValue(ref, keys, group, value);
        if (GetProjectStateChangeCount(nullptr) != c0)
            SetLast(project ? "Set project sound value" : "Set character sound value", where, what, n);
        return rep;
    });
}

void PickLibrary(const Frame& f, const std::vector<const av::Target*>& sel, const std::string& arg, const std::string& what)
{
    if (f.keyed) {
        PickKeyLibrary(f, sel, arg, what);
        return;
    }
    MediaItem*               item = f.item_ptr;
    std::vector<std::string> rids;
    std::vector<int>         blocks;
    RulesOf(f, sel, &rids, &blocks);
    LibState    state = LibState::Inherit;
    std::string uid;
    if (arg == kLibNoneWord) state = LibState::None;
    else if (!arg.empty()) {
        state = LibState::Library;
        uid = arg;
    }
    const std::string where = f.item_name;
    const int         n = static_cast<int>(sel.size());
    PickSoundGesture([item, rids, blocks, state, uid, what, where, n]() {
        bool              changed = false;
        const SoundReport rep = SetRulesAssignLibrary(item, rids, state, uid, blocks, &changed);
        if (changed) SetLast("Set rule library", where, what, n);
        return rep;
    });
}

void PickValue(const Frame& f, const std::vector<const av::Target*>& sel, bool item_row, const std::string& group,
               const std::string& value, const std::string& what)
{
    if (f.keyed) {
        PickKeyValue(f, sel, group, value, what);
        return;
    }
    MediaItem*               item = f.item_ptr;
    std::vector<std::string> rids;
    std::vector<int>         blocks;
    if (!item_row) RulesOf(f, sel, &rids, &blocks);
    const std::string where = f.item_name;
    const int         n = static_cast<int>(sel.size());
    PickSoundGesture([item, rids, blocks, group, value, what, where, n, item_row]() {
        bool              changed = true;  // SetSoundValue (the Item row) tells nothing: shown as before
        const SoundReport rep = item_row ? SetSoundValue(item, SoundValueLevel::Item, -1, 0.0, group, value)
                                         : SetRulesAssignValue(item, rids, group, value, blocks, &changed);
        if (changed) SetLast("Set sound value", where, what, n);
        return rep;
    });
}

// ---- Tree rows ---------------------------------------------------------------------------------------------

enum class RowT { Section, ItemRow, Parent, Leaf, Note, KitRow, CharRow, PartRow, TypeRow };

struct Row {
    RowT                  t = RowT::Note;
    std::string           id;      // section id / parent id / leaf key / "item"
    std::string           nav;     // keyboard id
    const av::Parent*     p = nullptr;
    const av::Leaf*       l = nullptr;
    std::string           title, scope_text, note;
    int                   count = -1;
    std::vector<std::string> section_ids;  // the fold ids of a section's parent rows (Expand all)
    // 13-15c: the Project / Character scopes' rows.
    std::string                key;      // the selection key ("kit:<uid>", "char:...", "feet", "feet.step")
    const SoundLibrary*        lib = nullptr;
    const av::CharEntry*       ce = nullptr;
    const BodyPart*            bp = nullptr;
    bool                       foldable = false;  // Left / Right fold it
    std::string                fold_id;
};

bool IsKeyRow(const Row& r)
{
    return r.t == RowT::KitRow || r.t == RowT::CharRow || r.t == RowT::PartRow || r.t == RowT::TypeRow;
}

bool IsClosed(const std::string& id)
{
    const auto it = g.closed.find(id);
    return it != g.closed.end() && it->second;
}
bool IsFolded(const std::string& id)
{
    const auto it = g.folded.find(id);
    return it != g.folded.end() && it->second;
}

bool ParentMatches(const av::Parent& p, const std::string& filter)
{
    if (av::FilterMatch(filter, p.name)) return true;
    for (const av::Leaf& l : p.leaves)
        if (av::FilterMatch(filter, l.name)) return true;
    return false;
}

std::vector<Row> BuildRows(const Frame& f)
{
    std::vector<Row> rows;
    const std::string filter = g.filter;
    if (av::FilterMatch(filter, "Item values")) {
        Row s;
        s.t = RowT::Section;
        s.id = "item";
        s.title = "Item";
        s.scope_text = f.pool_text;
        rows.push_back(s);
        if (!IsFolded("item")) {
            Row r;
            r.t = RowT::ItemRow;
            r.id = av::kItemKey;
            r.nav = av::kItemKey;
            rows.push_back(r);
        }
    }
    auto section = [&](const char* id, const char* title, const std::vector<av::Parent>& parents, const std::string& scope,
                       const char* empty) {
        std::vector<const av::Parent*> shown;
        for (const av::Parent& p : parents)
            if (ParentMatches(p, filter)) shown.push_back(&p);
        if (!filter.empty() && shown.empty()) return;
        Row s;
        s.t = RowT::Section;
        s.id = id;
        s.title = title;
        s.scope_text = scope;
        s.count = static_cast<int>(parents.size());
        for (const av::Parent* q : shown) s.section_ids.push_back(q->id);
        rows.push_back(s);
        if (IsFolded(id)) return;
        if (parents.empty()) {
            Row n;
            n.t = RowT::Note;
            n.note = empty;
            rows.push_back(n);
        }
        for (const av::Parent* p : shown) {
            Row r;
            r.t = RowT::Parent;
            r.id = p->id;
            r.nav = "P:" + p->id;
            r.p = p;
            r.foldable = p->leaves.size() > 1;
            r.fold_id = p->id;
            rows.push_back(r);
            if (IsClosed(p->id) || p->leaves.size() < 2) continue;
            for (const av::Leaf& l : p->leaves) {
                Row lr;
                lr.t = RowT::Leaf;
                lr.id = l.key;
                lr.nav = l.key;
                lr.p = p;
                lr.l = &l;
                rows.push_back(lr);
            }
        }
    };
    section("rules", "Bone rules", f.tg.rules, f.pool_text, "No bone rule on this item.");
    section("auto", "Auto detection", f.tg.autos, "this item's auto blocks", "No auto type ticked.");
    return rows;
}

// The Project scope's Library kits and Body parts, the Character scope's Characters and Body parts (13-15c).
std::vector<Row> BuildKeyRows(const Frame& f)
{
    std::vector<Row> rows;
    const std::string filter = g.filter;
    const SoundStore& store = *f.store;
    auto note = [&](const std::string& text) {
        Row n;
        n.t = RowT::Note;
        n.note = text;
        rows.push_back(n);
    };
    if (g.scope == kScopeProject) {
        std::vector<const SoundLibrary*> libs;
        for (const SoundLibrary& l : store.libraries)
            if (!l.kit.empty() && av::FilterMatch(filter, l.name)) libs.push_back(&l);
        if (filter.empty() || !libs.empty()) {
            Row s;
            s.t = RowT::Section;
            s.id = "kits";
            s.title = "Library kits";
            s.scope_text = "what each library says it serves";
            s.count = static_cast<int>(libs.size());
            rows.push_back(s);
            if (!IsFolded("kits")) {
                if (libs.empty()) note("No library suggests itself for a body part yet: set a kit from a library's \xE2\x8B\xAF menu (Kit...).");
                for (const SoundLibrary* l : libs) {
                    Row r;
                    r.t = RowT::KitRow;
                    r.key = "kit:" + l->uid;
                    r.id = r.nav = r.key;
                    r.lib = l;
                    rows.push_back(r);
                }
            }
        }
    } else {
        std::vector<const av::CharEntry*> cs;
        for (const av::CharEntry& c : f.key.chars)
            if (av::FilterMatch(filter, c.ch.name)) cs.push_back(&c);
        if (filter.empty() || !cs.empty()) {
            Row s;
            s.t = RowT::Section;
            s.id = "chars";
            s.title = "Characters";
            s.scope_text = "one per rig in this project";
            s.count = static_cast<int>(cs.size());
            rows.push_back(s);
            if (!IsFolded("chars")) {
                if (cs.empty()) note("No animation item in the project yet: its character appears here.");
                for (const av::CharEntry* c : cs) {
                    Row r;
                    r.t = RowT::CharRow;
                    r.key = c->key;
                    r.id = r.nav = r.key;
                    r.ce = c;
                    rows.push_back(r);
                }
            }
        }
    }
    // Body parts: every part, its types under it.
    std::vector<const BodyPart*> parts;
    for (const BodyPart& bp : BodyParts()) {
        bool any = av::FilterMatch(filter, bp.label);
        for (const PartType& t : bp.types)
            if (av::FilterMatch(filter, t.label)) any = true;
        if (any) parts.push_back(&bp);
    }
    if (!filter.empty() && parts.empty()) return rows;
    Row s;
    s.t = RowT::Section;
    s.id = "parts";
    s.title = "Body parts";
    s.scope_text = g.scope == kScopeProject ? std::string("project defaults, for every character")
                                            : f.char_name + ": only what differs from the project";
    s.count = static_cast<int>(parts.size());
    for (const BodyPart* bp : parts) s.section_ids.push_back("p:" + bp->id);
    rows.push_back(s);
    if (IsFolded("parts")) return rows;
    for (const BodyPart* bp : parts) {
        Row r;
        r.t = RowT::PartRow;
        r.key = bp->id;
        r.id = r.nav = r.key;
        r.bp = bp;
        r.foldable = true;
        r.fold_id = "p:" + bp->id;
        rows.push_back(r);
        if (IsClosed(r.fold_id)) continue;
        const bool whole = av::FilterMatch(filter, bp->label);
        for (const PartType& t : bp->types) {
            if (!whole && !av::FilterMatch(filter, t.label)) continue;
            Row tr;
            tr.t = RowT::TypeRow;
            tr.key = AssignKey(bp->id, t.word);
            tr.id = tr.nav = tr.key;
            tr.bp = bp;
            tr.title = t.label;
            rows.push_back(tr);
        }
    }
    return rows;
}

// The keys of the rows a click can land on (leaves, a single-leaf parent's leaf, the Item row), in order.
std::vector<std::string> VisibleKeys(const std::vector<Row>& rows)
{
    std::vector<std::string> out;
    for (const Row& r : rows) {
        if (r.t == RowT::ItemRow) out.push_back(av::kItemKey);
        else if (r.t == RowT::Leaf) out.push_back(r.l->key);
        else if (r.t == RowT::Parent && r.p->leaves.size() == 1) out.push_back(r.p->leaves[0].key);
        else if (IsKeyRow(r)) out.push_back(r.key);
    }
    return out;
}

std::vector<std::string> LeafKeysOf(const av::Parent& p)
{
    std::vector<std::string> out;
    for (const av::Leaf& l : p.leaves) out.push_back(l.key);
    return out;
}

std::vector<const av::Target*> TargetsOfParent(const Frame& f, const av::Parent& p)
{
    std::vector<const av::Target*> out;
    for (const av::Leaf& l : p.leaves) out.push_back(&f.targets[static_cast<size_t>(l.block)]);
    return out;
}

void NoteView(const std::string& text)
{
    g.last_note = text;
    g.has_last = false;
}

// "Expand all" / "Collapse all": view only, no undo point.
void SectionAll(const std::string& id, const std::vector<std::string>& fold_ids)
{
    bool all_open = !IsFolded(id);
    for (const std::string& q : fold_ids)
        if (IsClosed(q)) all_open = false;
    g.folded[id] = false;
    for (const std::string& q : fold_ids) g.closed[q] = all_open;
    NoteView(std::string(all_open ? "Collapsed" : "Expanded") + " every row of " +
             (id == "rules" ? "Bone rules" : id == "auto" ? "Auto detection" : "Body parts") + " (view only, no undo point).");
}

void ActivateRow(const Row& r, const std::vector<std::string>& visible, bool shift, bool ctrl)
{
    if (IsKeyRow(r)) {
        av::ClickRow(g.sel, r.key, shift, ctrl, visible);
        if (r.t == RowT::CharRow && r.ce) {  // a character picked is the current one
            g.cur_skel = r.ce->ch.skeleton;
            g.cur_model = r.ce->ch.model;
        }
    } else if (r.t == RowT::ItemRow) av::ClickRow(g.sel, av::kItemKey, shift, ctrl, visible);
    else if (r.t == RowT::Leaf) av::ClickRow(g.sel, r.l->key, shift, ctrl, visible);
    else if (r.t == RowT::Parent) {
        if (r.p->leaves.size() == 1) av::ClickRow(g.sel, r.p->leaves[0].key, shift, ctrl, visible);
        else av::ClickParent(g.sel, LeafKeysOf(*r.p), shift || ctrl);
    }
}

void DrawSectionHeader(const Row& r)
{
    ImGui::PushID(r.id.c_str());
    const float  h = ui::SectionHeaderH();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const bool   folded = IsFolded(r.id);
    float        x = p.x;
    ImGui::SetCursorScreenPos(ImVec2(x, p.y + (h - ui::Dp(18.0f)) * 0.5f));
    if (ui::Chevron("##fold", !folded)) g.folded[r.id] = !folded;
    ui::ItemTipText(folded ? "Open this section" : "Fold this section");
    x += ui::Dp(22.0f);
    // "Expand all / Collapse all" at the right, for the sections that hold parent rows.
    float right = p.x + w;
    if (!r.section_ids.empty()) {
        bool all_open = !folded;
        for (const std::string& q : r.section_ids)
            if (IsClosed(q)) all_open = false;
        const char* label = all_open ? "\xE2\x8A\x9F Collapse all##all" : "\xE2\x8A\x9E Expand all##all";
        const float bw = ui::ButtonWidth(label);
        if (right - bw >= x + ui::Dp(90.0f)) {
            ImGui::SetCursorScreenPos(ImVec2(right - bw, p.y + (h - ui::ControlH()) * 0.5f));
            if (ui::SolidButton(label)) SectionAll(r.id, r.section_ids);
            right -= bw + ui::Dp(6.0f);
        }
    }
    {
        ui::FontScope fs(ui::FontRole::Small);
        std::string   up = r.title;
        for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        ImGui::SetCursorScreenPos(ImVec2(x, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f));
        dl->AddText(ImVec2(x, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), ui::kMuted, up.c_str());
        x += ImGui::CalcTextSize(up.c_str()).x + ui::Dp(6.0f);
    }
    if (r.count >= 0) {
        ui::FontScope fs(ui::FontRole::MonoTiny);
        const std::string n = std::to_string(r.count);
        dl->AddText(ImVec2(x, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), ui::kFaint, n.c_str());
        x += ImGui::CalcTextSize(n.c_str()).x + ui::Dp(8.0f);
    }
    if (x < right - ui::Dp(30.0f)) {
        ui::FontScope fs(ui::FontRole::Small);
        ui::EllipsisTextTip(dl, ImVec2(x, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), right, ui::kFaint, r.scope_text);
    }
    dl->AddLine(ImVec2(p.x, p.y + h - 1.0f), ImVec2(p.x + w, p.y + h - 1.0f), folded ? 0 : ui::kStroke);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopID();
}

// Name column of a row, ellipsized: [x, x + max_w].
void RowName(ImDrawList* dl, float x, float y, float max_w, ImU32 col, const std::string& name)
{
    ui::EllipsisTextTip(dl, ImVec2(x, y), x + max_w, col, name);
}

// One kit / character / body part / type row of the Project and Character scopes (13-15c); the caller has the
// row's rect (p, w, rh), its text line y and the keyboard bit.
void DrawKeyRow(const Frame& f, const Row& r, const std::vector<std::string>& visible, unsigned bits, ImVec2 p, float w, float rh,
                float ty)
{
    ImDrawList*    dl = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const float    pad = ui::Dp(6.0f);
    const float    right = p.x + w - pad;
    const bool     sel = g.sel.Has(r.key);
    const bool     current = r.t == RowT::CharRow && r.ce && f.key.cur >= 0 && r.ce == &f.key.chars[static_cast<size_t>(f.key.cur)];
    ImGui::SetNextItemAllowOverlap();
    if (ui::SelectRow("##row", sel, current && !sel ? (bits | ui::kRowCurrent) : bits, rh, ImGuiSelectableFlags_AllowOverlap, ui::kRadiusMd)) {
        g.nav = r.nav;
        ActivateRow(r, visible, io.KeyShift, io.KeyCtrl);
    }
    auto summary_at = [&](float x, const av::Summary& sum) {
        if (x >= right - ui::Dp(20.0f)) return;
        if (PaintSummary(dl, x, ty, right - x, sum, true)) {
            ImGui::SetCursorScreenPos(ImVec2(x, p.y));
            ImGui::Dummy(ImVec2(right - x, rh));
            ui::ItemTipText(sum.Plain().c_str());
        }
    };
    if (r.t == RowT::KitRow) {
        const SoundLibrary& L = *r.lib;
        float               x = p.x + pad;
        dl->AddCircleFilled(ImVec2(x + ui::Dp(4.0f), p.y + rh * 0.5f), ui::Dp(4.0f), ui::LibDot(L.color));
        x += ui::Dp(14.0f);
        const float name_w = std::min(ImGui::CalcTextSize(L.name.c_str()).x + 1.0f, (right - x) * 0.4f);
        RowName(dl, x, ty, name_w, ui::kText, L.name);
        x += name_w + ui::Dp(8.0f);
        av::Summary sum;
        sum.segs = av::KitRowSegs(av::KitParts(*f.store, L, f.key.chars));
        summary_at(x, sum);
    } else if (r.t == RowT::CharRow) {
        const av::CharEntry& c = *r.ce;
        float                x = p.x + pad + ui::Dp(4.0f);
        const std::string    tag = c.overrides > 0 ? std::to_string(c.overrides) + (c.overrides > 1 ? " overrides" : " override")
                                                   : std::string("inherits the project");
        float                tag_w = c.overrides > 0 ? ui::TagWidth(tag.c_str()) : ImGui::CalcTextSize(tag.c_str()).x;
        const bool           tag_fits = right - tag_w > x + ui::Dp(90.0f);
        const float          lim = tag_fits ? right - tag_w - ui::Dp(8.0f) : right;
        {
            ui::FontScope fs(current ? ui::FontRole::Semibold : ui::FontRole::Body);
            const float   name_w = std::min(ImGui::CalcTextSize(c.ch.name.c_str()).x + 1.0f, (lim - x) * 0.5f);
            RowName(dl, x, ty, name_w, ui::kText, c.ch.name);
            x += name_w + ui::Dp(8.0f);
        }
        std::string hint = "rig " + av::RigSig(c.ch.skeleton);
        if (!c.ch.model.empty() && !c.models.empty() && !c.models[0].files.empty()) hint += kDot + c.models[0].files.front();
        hint += kDot + std::to_string(c.items) + (c.items == 1 ? " item" : " items");
        if (x < lim - ui::Dp(30.0f)) {
            ui::FontScope fs(ui::FontRole::Small);
            ui::EllipsisTextTip(dl, ImVec2(x, std::floor(p.y + (rh - ImGui::GetTextLineHeight()) * 0.5f)), lim, ui::kFaint, hint);
        }
        if (tag_fits) {
            if (c.overrides > 0) ui::DrawTag(dl, ImVec2(right - tag_w, p.y + (rh - ui::TagHeight()) * 0.5f), tag.c_str(), ui::TagTone::Project);
            else dl->AddText(ImVec2(right - tag_w, ty), ui::kFaint, tag.c_str());
        }
    } else if (r.t == RowT::PartRow) {
        const auto it = f.key.targets.find(r.key);
        if (it == f.key.targets.end()) return;
        const av::Target& t = it->second;
        float             x = p.x + pad;
        ImGui::SetCursorScreenPos(ImVec2(x, p.y + (rh - ui::Dp(18.0f)) * 0.5f));
        if (ui::Chevron("##chev", !IsClosed(r.fold_id))) g.closed[r.fold_id] = !IsClosed(r.fold_id);
        ui::ItemTipText(IsClosed(r.fold_id) ? "Show the types" : "Hide the types");
        x += ui::Dp(18.0f) + ui::Dp(6.0f);
        const float name_w = std::min(ImGui::CalcTextSize(r.bp->label.c_str()).x + 1.0f, (right - x) * 0.4f);
        RowName(dl, x, ty, name_w, ui::kText, r.bp->label);
        x += name_w + ui::Dp(6.0f);
        av::Summary sum = av::Summarize(*f.store, {&t}, true);
        if (sum.blocked) {
            ui::AddTextIn(dl, ui::FontSemibold(), ImVec2(x, ty), ui::kWarn, "!");
            x += ui::Dp(10.0f);
        }
        if (t.OwnLib() && t.own_kit && x < right - ui::Dp(60.0f)) {
            x += ui::DrawTag(dl, ImVec2(x, p.y + (rh - ui::TagHeight()) * 0.5f), "kit", ui::TagTone::Project) + ui::Dp(6.0f);
        }
        if (!t.OwnLib() && !t.Unset()) sum.segs.push_back({"  (" + t.ea.from + ")", av::SegTone::Faint});
        summary_at(x, sum);
    } else {  // a type of a part
        const auto it = f.key.targets.find(r.key);
        if (it == f.key.targets.end()) return;
        const float x = p.x + ui::Dp(50.0f);
        const float name_w = std::min(ImGui::CalcTextSize(r.title.c_str()).x + 1.0f, std::max(ui::Dp(30.0f), (right - x) * 0.35f));
        RowName(dl, x, ty, name_w, sel ? ui::kText : ui::kMuted, r.title);
        av::Summary sum;
        if (f.key.own.count(r.key)) sum = av::Summarize(*f.store, {&it->second}, true);
        else sum.segs.push_back({"inherit", av::SegTone::Faint});
        summary_at(x + name_w + ui::Dp(8.0f), sum);
    }
}

void DrawTreeRows(const Frame& f, const std::vector<Row>& rows)
{
    const std::vector<std::string> visible = VisibleKeys(rows);
    const ImGuiIO&                 io = ImGui::GetIO();
    for (const Row& r : rows) {
        if (r.t == RowT::Section) {
            if (&r != &rows[0]) ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
            DrawSectionHeader(r);
            continue;
        }
        if (r.t == RowT::Note) {
            ImGui::Dummy(ImVec2(0.0f, ui::Dp(2.0f)));
            ui::SubText(r.note.c_str());
            continue;
        }
        ImGui::PushID(r.nav.c_str());
        const float  rh = ui::RowH();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        ImDrawList*  dl = ImGui::GetWindowDrawList();
        const float  ty = std::floor(p.y + (rh - ImGui::GetTextLineHeight()) * 0.5f);
        const float  pad = ui::Dp(6.0f);
        const bool   is_nav = g.nav_shown && g.nav == r.nav;
        if (is_nav && g.nav_scroll) {
            ImGui::SetScrollHereY(0.5f);
            g.nav_scroll = false;
        }
        const unsigned bits = is_nav ? ui::kRowCurrent : 0u;

        if (IsKeyRow(r)) {
            DrawKeyRow(f, r, visible, bits, p, w, rh, ty);
        } else if (r.t == RowT::Parent) {
            const std::vector<std::string> keys = LeafKeysOf(*r.p);
            const int                      state = av::ParentState(g.sel, keys);
            const bool                     multi = r.p->leaves.size() > 1;
            const std::vector<const av::Target*> tl = TargetsOfParent(f, *r.p);
            const av::Summary                    sum = av::Summarize(*f.store, tl, true);
            ImGui::SetNextItemAllowOverlap();
            if (ui::SelectRow("##row", state == 1, bits, rh, ImGuiSelectableFlags_AllowOverlap, ui::kRadiusMd)) {
                g.nav = r.nav;
                ActivateRow(r, visible, io.KeyShift, io.KeyCtrl);
            }
            float x = p.x + pad;
            if (multi) {
                ImGui::SetCursorScreenPos(ImVec2(x, p.y + (rh - ui::Dp(18.0f)) * 0.5f));
                if (ui::Chevron("##chev", !IsClosed(r.id))) g.closed[r.id] = !IsClosed(r.id);
                ui::ItemTipText(IsClosed(r.id) ? "Show the sides" : "Hide the sides");
            }
            x += ui::Dp(18.0f) + ui::Dp(6.0f);
            ImGui::SetCursorScreenPos(ImVec2(x, p.y + (rh - ImGui::GetFrameHeight()) * 0.5f));
            if (ui::TriCheckbox("##chk", state)) {
                g.nav = r.nav;
                if (multi) av::ClickParent(g.sel, keys, true);
                else av::ClickRow(g.sel, keys[0], false, true, visible);
            }
            ui::ItemTipText(multi ? "Select both sides" : "Select this rule");
            x += ui::Dp(14.0f) + ui::Dp(7.0f);
            const float name_w = std::min(ImGui::CalcTextSize(r.p->name.c_str()).x + 1.0f, (p.x + w - x) * 0.45f);
            RowName(dl, x, ty, name_w, ui::kText, r.p->name);
            x += name_w + ui::Dp(6.0f);
            if (sum.blocked) {
                ui::AddTextIn(dl, ui::FontSemibold(), ImVec2(x, ty), ui::kWarn, "!");
                x += ui::Dp(10.0f);
            }
            if (x < p.x + w - pad - ui::Dp(20.0f)) {
                if (PaintSummary(dl, x, ty, p.x + w - pad - x, sum, true)) {
                    ImGui::SetCursorScreenPos(ImVec2(x, p.y));
                    ImGui::Dummy(ImVec2(p.x + w - pad - x, rh));
                    ui::ItemTipText(sum.Plain().c_str());
                }
            }
        } else if (r.t == RowT::Leaf) {
            const av::Target& t = f.targets[static_cast<size_t>(r.l->block)];
            const bool        sel = g.sel.Has(r.l->key);
            const av::Summary sum = av::Summarize(*f.store, {&t}, true);
            ImGui::SetNextItemAllowOverlap();
            if (ui::SelectRow("##row", sel, bits, rh, ImGuiSelectableFlags_AllowOverlap, ui::kRadiusMd)) {
                g.nav = r.nav;
                ActivateRow(r, visible, io.KeyShift, io.KeyCtrl);
            }
            const float x = p.x + ui::Dp(50.0f);
            const float name_w = std::min(ui::Dp(72.0f), std::max(ui::Dp(30.0f), (p.x + w - x) * 0.3f));
            RowName(dl, x, ty, name_w, sel ? ui::kText : ui::kMuted, r.l->name);
            const float sx = x + name_w + ui::Dp(8.0f);
            if (sx < p.x + w - pad - ui::Dp(20.0f)) {
                if (PaintSummary(dl, sx, ty, p.x + w - pad - sx, sum, true)) {
                    ImGui::SetCursorScreenPos(ImVec2(sx, p.y));
                    ImGui::Dummy(ImVec2(p.x + w - pad - sx, rh));
                    ui::ItemTipText(sum.Plain().c_str());
                }
            }
        } else {  // the Item values row
            const bool sel = g.sel.Has(av::kItemKey);
            ImGui::SetNextItemAllowOverlap();
            if (ui::SelectRow("##row", sel, bits, rh, ImGuiSelectableFlags_AllowOverlap, ui::kRadiusMd)) {
                g.nav = r.nav;
                ActivateRow(r, visible, io.KeyShift, io.KeyCtrl);
            }
            const av::Summary sum = av::SummarizeItem(*f.store, f.item);
            float             x = p.x + pad + ui::Dp(4.0f);
            const float       name_w = std::min(ImGui::CalcTextSize("Item values").x + 1.0f, (p.x + w - x) * 0.45f);
            RowName(dl, x, ty, name_w, ui::kText, "Item values");
            x += name_w + ui::Dp(8.0f);
            if (x < p.x + w - pad - ui::Dp(20.0f)) PaintSummary(dl, x, ty, p.x + w - pad - x, sum, true);
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rh));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::PopID();
    }
}

// Up / Down move the keyboard row, Left / Right fold a parent, Enter clicks it (Space stays REAPER's play).
void HandleTreeKeys(const std::vector<Row>& rows)
{
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::GetIO().WantTextInput ||
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
        return;
    std::vector<const Row*> navs;
    for (const Row& r : rows)
        if (!r.nav.empty()) navs.push_back(&r);
    if (navs.empty()) return;
    int cur = -1;
    for (size_t i = 0; i < navs.size(); ++i)
        if (navs[i]->nav == g.nav) cur = static_cast<int>(i);
    const ImGuiIO& io = ImGui::GetIO();
    const bool down = ImGui::IsKeyPressed(ImGuiKey_DownArrow), up = ImGui::IsKeyPressed(ImGuiKey_UpArrow);
    if (down || up) {
        if (cur < 0) cur = 0;
        else cur = std::max(0, std::min(static_cast<int>(navs.size()) - 1, cur + (down ? 1 : -1)));
        g.nav = navs[static_cast<size_t>(cur)]->nav;
        g.nav_shown = true;
        g.nav_scroll = true;
        return;
    }
    if (cur < 0) return;
    const Row& r = *navs[static_cast<size_t>(cur)];
    if (r.foldable) {
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) g.closed[r.fold_id] = false;
        else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) g.closed[r.fold_id] = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
        ActivateRow(r, VisibleKeys(rows), io.KeyShift, io.KeyCtrl);
}

// ---- Details ----------------------------------------------------------------------------------------------

void OpenLibraryViewUid(const std::string& uid)
{
    if (const SoundLibrary* l = FindLibrary(GetSoundStore(nullptr), uid)) SelectLibraryRow(l->uid, LibraryRow{std::string(), l->root});
    SetViewMode(ViewMode::Library);
}

void OpenLibraryView(const av::Target* t)
{
    if (t && t->lib) OpenLibraryViewUid(t->lib->uid);
    else SetViewMode(ViewMode::Library);
}

void Banner(bool warn, const std::string& text, const av::Target* open_for)
{
    if (warn) {
        ui::ConfirmBand band;
        ui::BeginConfirmBand(band, 0.0f, true);
        ui::ToneText(text, ui::kWarn, true);
        if (open_for && ui::SolidButton("Open Library view##banner")) OpenLibraryView(open_for);
        ui::EndConfirmBand(band);
    } else {
        ui::Well well;
        ui::BeginWell(well);
        ui::ToneText(text, ui::kMuted, true);
        ui::EndWell(well);
    }
}

std::string LibraryLabel(const SoundStore& store, const std::string& arg)
{
    if (arg.empty()) return "inherit";
    if (arg == kLibNoneWord) return "None";
    const SoundLibrary* l = FindLibrary(store, arg);
    return l ? l->name : std::string("(gone)");
}

// "inherit . FS Hero (Project . Feet)" for the targets (their inherited assignment); "Not set (nothing placed)".
std::string InheritLabel(const SoundStore& store, const std::vector<const av::Target*>& sel)
{
    const EffectiveAssign& a = sel[0]->inherited;
    for (const av::Target* t : sel)
        if (t->inherited.library != a.library || t->inherited.none != a.none || t->inherited.from != a.from) return "inherit \xC2\xB7 mixed";
    if (a.level == AssignLevel::Unset) return "Not set (nothing placed)";
    const std::string what = a.none ? std::string("None") : LibraryLabel(store, a.library);
    return "inherit \xC2\xB7 " + what + " (" + a.from + ")";
}

void DrawLibraryRow(const Frame& f, const std::vector<const av::Target*>& sel, const av::Mix& mix)
{
    const SoundStore& store = *f.store;
    std::string       own = av::OwnLibraryArg(*sel[0]);
    const bool        own_mixed = mix.own_settings > 1;
    const float       w = ui::LabelRow("Library", ui::Dp(140.0f));
    std::string       preview;
    if (own_mixed) preview = "mixed (" + std::to_string(mix.own_settings) + " settings)";
    else if (own.empty()) preview = InheritLabel(store, sel);
    else if (own == kLibNoneWord) preview = "None (place nothing)";
    else preview = LibraryLabel(store, own);
    if (ui::BeginComboStyled("##assignlib", preview.c_str(), w, 0, own_mixed ? ui::TagTone::Warn : ui::TagTone::Neutral)) {
        if (own_mixed) {
            ImGui::BeginDisabled();
            ImGui::Selectable((preview + "##mixed").c_str(), true);
            ImGui::EndDisabled();
        }
        const std::string inh = InheritLabel(store, sel);
        if (ImGui::Selectable((inh + "##inherit").c_str(), !own_mixed && own.empty()) && (own_mixed || !own.empty()))
            PickLibrary(f, sel, std::string(), "Library = inherit");
        if (ImGui::Selectable("None (place nothing)##none", !own_mixed && own == kLibNoneWord) && (own_mixed || own != kLibNoneWord))
            PickLibrary(f, sel, kLibNoneWord, "Library = None");
        for (const SoundLibrary& L : store.libraries) {
            std::string label = L.name;
            bool        has_sound = false;
            for (const SoundNode& n : L.nodes)
                if (n.type == SoundNodeType::Sound) has_sound = true;
            if (!has_sound) label += " (empty)";
            else if (CheckLibrary(store, L).blocked) label += " (blocked)";
            const bool cur = !own_mixed && own == L.uid;
            if (ImGui::Selectable((label + "##" + L.uid).c_str(), cur) && !cur) PickLibrary(f, sel, L.uid, "Library = " + L.name);
        }
        ui::EndComboStyled();
    }
    // Where the setting is: set here, from a kit, inherited.
    if (sel.size() == 1 && !mix.lib) {
        const av::Target& t = *sel[0];
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        if (t.OwnLib()) {
            ui::Tag(t.ea.kit && t.lib ? ("from " + t.lib->name + "'s kit").c_str() : "set here", ui::TagTone::Project);
        } else {
            ui::ToneText(t.Unset() ? std::string("nothing set at any level") : "inherited from " + t.ea.from, ui::kFaint, true);
        }
    }
}

// One Switch Group: its step number, name, tags, the chips, the event note.
void DrawGroupRow(const Frame& f, const std::vector<const av::Target*>& sel, bool item_row, const std::string& group_uid, int step,
                  bool first)
{
    const av::GroupState* g0 = nullptr;
    for (const av::Target* t : sel)
        if ((g0 = t->Group(group_uid))) break;
    if (!g0) return;
    ImGui::PushID(group_uid.c_str());
    if (!first) ImGui::Separator();
    if (g0->deleted) {
        ImGui::AlignTextToFramePadding();
        char n[16];
        std::snprintf(n, sizeof(n), "%d", step);
        ui::ToneText(n, ui::kFaint);
        ImGui::SameLine();
        ui::ToneText(g0->group_name.empty() ? std::string("(deleted group)") : g0->group_name, ui::kMuted);
        ImGui::SameLine();
        ui::Tag("group deleted", ui::TagTone::Warn);
        ui::ToneText("The Switch Group no longer exists. Re-create it or change the Switch in the Library view.", ui::kFaint, true);
        ImGui::PopID();
        return;
    }
    const SwitchGroup*       grp = FindGroup(*f.store, group_uid);
    std::vector<std::string> own;
    int                      unreached = 0;
    bool                     inh_same = true;
    for (const av::Target* t : sel) {
        const av::GroupState* gs = t->Group(group_uid);
        own.push_back(gs ? gs->own_uid : std::string());
        if (gs && !gs->reached) ++unreached;
        if (gs && (gs->inherit_name != g0->inherit_name || gs->inherit_from != g0->inherit_from)) inh_same = false;
    }
    const bool mixed = std::set<std::string>(own.begin(), own.end()).size() > 1;
    auto       count_of = [&](const std::string& v) {
        return static_cast<int>(std::count(own.begin(), own.end(), v));
    };
    // Header line.
    ImGui::AlignTextToFramePadding();
    char n[16];
    std::snprintf(n, sizeof(n), "%d", step);
    {
        ui::FontScope fs(ui::FontRole::MonoTiny);
        ui::ToneText(n, ui::kFaint);
    }
    ImGui::SameLine();
    {
        ui::FontScope fs(ui::FontRole::Semibold);
        ui::ToneText(g0->group_name, ui::kMuted);
    }
    if (mixed) {
        ImGui::SameLine();
        ui::Tag("mixed", ui::TagTone::Warn);
    }
    if (unreached > 0) {
        const av::GroupState* un = nullptr;
        for (const av::Target* t : sel) {
            const av::GroupState* gs = t->Group(group_uid);
            if (gs && !gs->reached) {
                un = gs;
                break;
            }
        }
        std::string why = un->not_reached;
        if (unreached < static_cast<int>(sel.size())) why += " (" + std::to_string(unreached) + " of " + std::to_string(sel.size()) + ")";
        ImGui::SameLine();
        ui::ToneText(why, ui::kFaint);
    }
    // Chips.
    ImGui::Indent(ui::Dp(22.0f));
    if (unreached == static_cast<int>(sel.size())) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.45f);
    auto chip = [&](const std::string& label, const std::string& value, bool on, bool is_first) {
        const float cw = ui::TextWidth(label.c_str()) + ui::Dp(14.0f);
        if (!is_first) ui::SameLineIfFits(cw);
        if (ui::ChipButton((label + "##" + value).c_str(), ImVec2(0.0f, 0.0f), on) && !on) {
            std::string what = g0->group_name + " = " + (value.empty() ? std::string("inherit") : [&] {
                for (const SwitchValue& v : grp->values)
                    if (v.uid == value) return v.name;
                return value;
            }());
            PickValue(f, sel, item_row, group_uid, value, what);
        }
    };
    {
        const std::string inh = "inherit \xC2\xB7 " + (inh_same ? g0->inherit_name : std::string("mixed")) + " (" +
                                (inh_same ? g0->inherit_from : std::string("several")) + ")";
        chip(inh + (mixed && count_of(std::string()) ? "  " + std::to_string(count_of(std::string())) : std::string()), std::string(),
             !mixed && own[0].empty(), true);
    }
    if (grp)
        for (const SwitchValue& v : grp->values) {
            const int c = count_of(v.uid);
            chip(v.name + (mixed && c ? "  " + std::to_string(c) : std::string()), v.uid, !mixed && own[0] == v.uid, false);
        }
    if (unreached == static_cast<int>(sel.size())) ImGui::PopStyleVar();
    // Events that set their own value of this group.
    if (!item_row && !f.keyed) {
        std::vector<std::string> rids;
        std::vector<int>         blocks;
        RulesOf(f, sel, &rids, &blocks);
        const int ev = av::EventOverrideCount(f.rec, rids, group_uid);
        if (ev > 0)
            ui::ToneText(std::to_string(ev) + " event" + (ev > 1 ? "s" : "") + " set their own " + g0->group_name +
                             " (change them in the event inspector)",
                         ui::kFaint, true);
    }
    ImGui::Unindent(ui::Dp(22.0f));
    ImGui::PopID();
}

void DrawPlays(const std::vector<const av::Target*>& sel, bool for_defaults)
{
    const std::vector<av::PlaysLine> lines = av::PlaysLines(sel);
    bool                             blocked = false;
    for (const av::Target* t : sel)
        if (t->lib_blocked) blocked = true;
    ui::ConfirmBand band;
    ui::Well        well;
    if (blocked) ui::BeginConfirmBand(band, 0.0f, true);
    else ui::BeginWell(well);
    ui::GroupCaption(for_defaults ? "Plays (for an item that sets nothing)" : "Plays");
    const size_t shown = std::min<size_t>(lines.size(), 4);
    for (size_t i = 0; i < shown; ++i) {
        ImGui::PushID(static_cast<int>(i));
        ui::ToneText(lines[i].text, ui::kText, true);
        if (lines.size() > 1) {
            std::string who;
            if (lines[i].titles.size() > 2) who = std::to_string(lines[i].titles.size()) + " targets";
            else
                for (const std::string& t : lines[i].titles) who += (who.empty() ? "" : ", ") + t;
            ui::ToneText(who, ui::kFaint, true);
        }
        ImGui::PopID();
    }
    if (lines.size() > shown) ui::ToneText("+ " + std::to_string(lines.size() - shown) + " more paths", ui::kFaint);
    if (sel.size() == 1 && sel[0]->Plays()) ui::ToneText(av::WhyText(*sel[0]), ui::kFaint, true);
    if (blocked) ui::ToneText("Populate reports \"Blocked\" for these events.", ui::kFaint, true);
    if (blocked) ui::EndConfirmBand(band);
    else ui::EndWell(well);
}

void LadderRowDraw(const SoundStore& store, const std::string& label, bool hit, const std::function<void(ImDrawList*, float, float, float)>& value)
{
    const float  h = ui::ControlH() + ui::Dp(4.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    (void)store;
    if (hit) ui::RowBackground(dl, p, ImVec2(p.x + w, p.y + h), ui::kRowLit, ui::kRadiusSm);
    const float ty = std::floor(p.y + (h - ImGui::GetTextLineHeight()) * 0.5f);
    const float kw = std::min(ui::Dp(128.0f), w * 0.4f);
    ui::EllipsisTextTip(dl, ImVec2(p.x + ui::Dp(6.0f), ty), p.x + kw, hit ? ui::kAccentInk : ui::kMuted, label);
    value(dl, p.x + kw + ui::Dp(8.0f), ty, p.x + w - ui::Dp(6.0f));
    ImGui::Dummy(ImVec2(w, h));
}

void DrawLadder(const Frame& f, const av::Target& t)
{
    const av::Ladder ld = av::BuildLadder(*f.store, f.keyed ? f.key.scope_ch : f.ch, t, !f.keyed);
    ui::Section      s;
    ui::SectionOpts  o;
    o.suffix = "top = general \xC2\xB7 bottom = precise";
    if (ui::BeginSection(s, "Where the library comes from", o)) {
        std::string kits;
        for (const std::string& k : ld.kits) kits += (kits.empty() ? "" : ", ") + k;
        LadderRowDraw(*f.store, "Library kits", false, [&](ImDrawList* dl, float x, float y, float right) {
            ui::EllipsisTextTip(dl, ImVec2(x, y), right, ui::kFaint, ld.kits.empty() ? std::string("none") : kits + " suggest " + ld.part_label);
        });
        for (const av::LadderRow& r : ld.rows) {
            LadderRowDraw(*f.store, r.label, r.hit, [&](ImDrawList* dl, float x, float y, float right) {
                if (!r.set) {
                    dl->AddText(ImVec2(x, y), ui::kFaint, "\xE2\x80\x94");
                    return;
                }
                std::string name = "None";
                if (!r.none) {
                    const SoundLibrary* l = FindLibrary(*f.store, r.library);
                    name = l ? l->name : std::string("(gone)");
                    if (l) dl->AddCircleFilled(ImVec2(x + ui::Dp(4.0f), y + ImGui::GetTextLineHeight() * 0.5f), ui::Dp(4.0f), ui::LibDot(l->color));
                }
                const float nx = x + (r.none ? 0.0f : ui::Dp(14.0f));
                std::string text = name + (r.kit ? "  (kit)" : "") + (r.hit ? "  \xE2\x86\x90 plays" : "");
                ui::EllipsisTextTip(dl, ImVec2(nx, y), right, ui::kText, text);
            });
        }
        if (!f.keyed)
            LadderRowDraw(*f.store, "event", false, [&](ImDrawList* dl, float x, float y, float right) {
                ui::EllipsisTextTip(dl, ImVec2(x, y), right, ui::kFaint, "values only, in the event inspector");
            });
    }
    ui::EndSection(s);
}

// ---- Kit and character sheets (13-15c) ---------------------------------------------------------------------

// The title of a sheet: a pane title wrapped, then (same line) its tag.
void SheetTitle(const std::string& title, const char* tag, ImU32 dot)
{
    {
        ui::FontScope fs(ui::FontRole::PaneTitle);
        float         x = 0.0f;
        if (dot) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float  lh = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + ui::Dp(5.0f), p.y + lh * 0.5f), ui::Dp(5.0f), dot);
            x = ui::Dp(16.0f);
            ImGui::SetCursorScreenPos(ImVec2(p.x + x, p.y));
        }
        ui::ToneText(title, ui::kText, true);
        if (dot) ImGui::SetCursorPosX(ImGui::GetCursorPosX() - x);
    }
    if (tag) {
        ImGui::SameLine();
        ui::Tag(tag, ui::TagTone::Project);
    }
}

void PickAdopt(const std::string& lib_uid, const std::string& part, const CharacterRef& ref, bool replace, const std::string& where,
               const std::string& what)
{
    PickSoundGesture([lib_uid, part, ref, replace, where, what]() {
        const int         c0 = GetProjectStateChangeCount(nullptr);
        const SoundReport rep = AdoptKit(lib_uid, part, ref, replace);
        if (GetProjectStateChangeCount(nullptr) != c0) SetLast("Use library kit", where, what, 1);
        return rep;
    });
}

void DrawKitDetails(const Frame& f)
{
    const std::string   uid = g.sel.keys[0].substr(4);
    const SoundLibrary* L = FindLibrary(*f.store, uid);
    if (!L || L->kit.empty()) {
        ui::ToneText("This library has no kit any more.", ui::kMuted, true);
        return;
    }
    SheetTitle(L->name, "kit", ui::LibDot(L->color));
    ui::ToneText("stored in the library: it travels with its .ravlib and the User Library", ui::kMuted, true);
    ui::ToneText("A kit is a suggestion: it plays nothing until this project (or one of its characters) uses it. Edit it in the "
                 "Library view (the library's \xE2\x8B\xAF menu, Kit...).",
                 ui::kFaint, true);
    const std::vector<av::KitPart> parts = av::KitParts(*f.store, *L, f.key.chars);
    for (const av::KitPart& kp : parts) {
        ImGui::PushID(kp.part.c_str());
        const std::string roles = "roles: " + av::KitPartRoles(kp.part);
        ui::Section       s;
        ui::SectionOpts   o;
        o.suffix = roles.c_str();
        if (ui::BeginSection(s, PartLabel(kp.part).c_str(), o)) {
            const bool taken = kp.state != av::KitPart::State::ProjectDefault && PartKeySet(f.store->project, kp.part);
            if (kp.state == av::KitPart::State::ProjectDefault) {
                ui::Tag("project default", ui::TagTone::Project);
            } else if (taken) {
                const SoundLibrary* other = FindLibrary(*f.store, kp.project_lib);
                ui::Tag(("project uses " + (other ? other->name : std::string("None"))).c_str(), ui::TagTone::Warn);
            } else {
                ui::Tag("not used");
            }
            for (const std::string& c : kp.chars) {
                ui::SameLineIfFits(ui::TagWidth(c.c_str()));
                ui::Tag(c.c_str(), ui::TagTone::Project);
            }
            ui::ToneText(av::KitPartWhat(kp.part), ui::kMuted, true);
            const std::string where = "Project", what = PartLabel(kp.part) + " = " + L->name;
            if (kp.state != av::KitPart::State::ProjectDefault) {
                const char* label = taken ? "Replace as project default##adopt" : "Use as project default##adopt";
                if (taken ? ui::SolidButton(label) : ui::PrimaryButton(label)) PickAdopt(uid, kp.part, CharacterRef(), taken, where, what);
                ui::SameLineIfFits(ui::Dp(150.0f));
            }
            // "Use for this character": the picked character gets the library for this part.
            const av::CharEntry* pick = nullptr;
            for (const av::CharEntry& c : f.key.chars)
                if (c.ch.skeleton == g.kit_skel && c.ch.model == g.kit_model) pick = &c;
            if (!pick && !f.key.chars.empty()) pick = &f.key.chars[static_cast<size_t>(std::max(0, f.key.cur))];
            if (!pick) ImGui::BeginDisabled();
            if (ui::BeginComboStyled("##kitchar", pick ? pick->ch.name.c_str() : "No character yet", ui::Dp(150.0f))) {
                for (const av::CharEntry& c : f.key.chars)
                    if (ImGui::Selectable((c.ch.name + "##" + c.key).c_str(), pick == &c)) {
                        g.kit_skel = c.ch.skeleton;
                        g.kit_model = c.ch.model;
                    }
                ui::EndComboStyled();
            }
            ui::SameLineIfFits(ui::ButtonWidth("Use for this character##usechar"));
            if (ui::SolidButton("Use for this character##usechar") && pick)
                PickAdopt(uid, kp.part, pick->ref, true, pick->ch.name, PartLabel(kp.part) + " = " + L->name);
            if (!pick) ImGui::EndDisabled();
        }
        ui::EndSection(s);
        ImGui::PopID();
    }
    ui::ToneText("When a library is added to the project, RAV offers its kit once (\"FS Hero serves Feet: use it?\"). It never "
                 "replaces a body part already set.",
                 ui::kFaint, true);
}

// "Feet \xC2\xB7 Step" as a link that selects that key in the tree.
void GoToKey(const std::string& key)
{
    std::string part, type;
    SplitAssignKey(key, &part, &type);
    g.sel.Clear();
    g.sel.keys.push_back(key);
    g.sel.anchor = g.sel.cursor = key;
    g.closed["p:" + part] = false;
    g.folded["parts"] = false;
    g.nav = key;
    g.nav_shown = true;
    g.nav_scroll = true;
}

// Ends the Name field's typing: a changed, non-empty name renames the character (one undo point).
void CommitName()
{
    if (!g.name_active) return;
    g.name_active = false;
    std::string name = g.name_buf;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    if (name.empty() || name == g.name_orig) return;  // the field reads the name again next frame
    if (EnumProjects(-1, nullptr, 0) != g.name_proj) return;  // typed in another project: never renames this one's
    const CharacterRef ref = g.name_ref;
    PickSoundGesture([ref, name]() {
        const int         c0 = GetProjectStateChangeCount(nullptr);
        const SoundReport rep = RenameCharacterRef(ref, name);
        if (GetProjectStateChangeCount(nullptr) != c0) SetLast("Rename character", name, "Name = " + name, 1);
        return rep;
    });
}

void DrawCharDetails(const Frame& f)
{
    const av::CharEntry* ce = nullptr;
    for (const av::CharEntry& c : f.key.chars)
        if (c.key == g.sel.keys[0]) ce = &c;
    if (!ce) {
        ui::ToneText("This character is no longer in the project.", ui::kMuted, true);
        return;
    }
    const SoundStore& store = *f.store;
    SheetTitle(ce->ch.name, nullptr, 0);
    ui::ToneText("rig " + av::RigSig(ce->ch.skeleton) + kDot + std::to_string(ce->items) + (ce->items == 1 ? " item" : " items") +
                     " in this project",
                 ui::kMuted, true);
    // Name: Enter or leaving the field renames it (one undo point); empty puts the name back. Review 13-15c:
    // leaving it by a click on another row, another scope or closing the window renames it too (CommitName).
    {
        if (!g.name_active) {
            g.name_for = ce->key;
            std::snprintf(g.name_buf, sizeof(g.name_buf), "%s", ce->ch.name.c_str());
        }
        const float w = ui::LabelRow("Name", ui::Dp(140.0f));
        ui::TextField("##charname", "Character name", g.name_buf, sizeof(g.name_buf), w,
                      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        g.name_min = ImGui::GetItemRectMin();
        g.name_max = ImGui::GetItemRectMax();
        g.name_frame = ImGui::GetFrameCount();
        if (ImGui::IsItemActive()) {
            if (!g.name_active) {
                g.name_ref = ce->ref;
                g.name_orig = ce->ch.name;
                g.name_proj = EnumProjects(-1, nullptr, 0);
            }
            g.name_active = true;
        } else if (g.name_active) {
            CommitName();
        }
    }
    ui::LabelRow("Recognised by", ui::Dp(140.0f));
    ui::ToneText("its skeleton: " + av::RigSig(ce->ch.skeleton) + kDot + std::to_string(ce->bones) + " bones (same key as the bone roles)",
                 ui::kMuted, true);
    {  // Model files
        ui::Section     s;
        ui::SectionOpts o;
        const std::string all = "all read as " + ce->ch.name;
        o.suffix = all.c_str();
        if (ui::BeginSection(s, "Model files", o)) {
            if (ce->models.empty()) ui::SubText("No animation item reads this character yet.");
            for (const av::RigModelIn& m : ce->models) {
                std::string files;
                for (const std::string& file : m.files) files += (files.empty() ? "" : ", ") + file;
                if (files.empty()) files = "(model not readable)";
                ImGui::PushID(m.model.c_str());
                ui::ToneText(files, ui::kText, true);
                ui::ToneText(std::to_string(m.items) + (m.items == 1 ? " item" : " items"), ui::kFaint, true);
                ImGui::PopID();
            }
            const bool can = ce->ch.model.empty() && ce->models.size() > 1;
            if (!can) ImGui::BeginDisabled();
            if (ui::SolidButton("Split by model file...##split") && can) {
                const CharacterRef ref = ce->ref;
                PickSoundGesture([ref]() {
                    const int         c0 = GetProjectStateChangeCount(nullptr);
                    const SoundReport rep = SplitRig(ref);
                    if (GetProjectStateChangeCount(nullptr) != c0)
                        SetLast("Split character", ref.name, "one character per model file", 1);
                    return rep;
                });
            }
            if (!can) ImGui::EndDisabled();
            ui::ItemTipText(can ? "Each model file becomes its own character, starting from this one's settings"
                                : "Needs two or more model files on this rig");
            ui::SameLineIfFits(ui::TextWidth("when two characters share this rig") + ui::Dp(8.0f));
            ui::ToneText("when two characters share this rig", ui::kFaint);
        }
        ui::EndSection(s);
    }
    {  // Differs from the project
        const std::vector<std::string> keys = av::OverrideKeys(ce->ch.assigns);
        ui::Section                    s;
        ui::SectionOpts                o;
        const std::string              count = std::to_string(keys.size());
        o.suffix = count.c_str();
        if (ui::BeginSection(s, "Differs from the project", o)) {
            if (keys.empty()) ui::SubText("Nothing: every item of this character reads the project defaults.");
            for (const std::string& key : keys) {
                const SoundAssign* a = FindAssign(ce->ch.assigns, key);
                if (!a) continue;
                ImGui::PushID(key.c_str());
                if (ui::LinkButton((AssignKeyLabel(key) + "##go").c_str())) GoToKey(key);
                std::string bits;
                if (a->state == LibState::None) bits = "None";
                else if (a->state == LibState::Library) {
                    const SoundLibrary* l = FindLibrary(store, a->library);
                    bits = l ? l->name : std::string("(gone)");
                }
                for (const ValueSet& v : a->values) {
                    if (v.value_uid.empty()) continue;
                    const SwitchGroup* grp = FindGroup(store, v.group_uid);
                    std::string        vn = v.value_uid;
                    if (grp)
                        for (const SwitchValue& sv : grp->values)
                            if (sv.uid == v.value_uid) vn = sv.name;
                    bits += (bits.empty() ? "" : ", ") + (grp ? grp->name : std::string("(deleted group)")) + " = " + vn;
                }
                ui::ToneText(bits, ui::kMuted, true);
                ImGui::PopID();
            }
        }
        ui::EndSection(s);
    }
}

// Under a Project / Character key's library row: the kits that also serve a Project part, the other characters.
void DrawKeyExtras(const Frame& f, const std::vector<const av::Target*>& sel)
{
    if (sel.size() != 1) return;
    const av::Target& t = *sel[0];
    if (!f.key.scope_ch && t.type.empty()) {
        for (const SoundLibrary* l : av::OtherKitsServing(*f.store, t.part, t.ea.library)) {
            ImGui::PushID(l->uid.c_str());
            ui::Well well;
            ui::BeginWell(well);
            ui::ToneText(l->name + "'s kit also serves " + PartLabel(t.part) +
                             ". A project has one default per body part: use it for a character instead.",
                         ui::kMuted, true);
            if (ui::SolidButton("See the kit##seekit")) {
                g.sel.Clear();
                g.sel.keys.push_back("kit:" + l->uid);
                g.sel.anchor = g.sel.cursor = g.sel.keys[0];
                g.folded["kits"] = false;
                g.nav = g.sel.keys[0];
            }
            ui::EndWell(well);
            ImGui::PopID();
        }
    }
    if (f.key.scope_ch) {
        const std::vector<av::OtherChar> others = av::OtherCharacters(*f.store, f.key.chars, static_cast<size_t>(std::max(0, f.key.cur)), t.key);
        if (!others.empty()) {
            ui::ToneText("Other characters:", ui::kMuted);
            for (const av::OtherChar& o : others) {
                LadderRowDraw(*f.store, o.name, false, [&](ImDrawList* dl, float x, float y, float right) {
                    const SoundLibrary* l = o.ea.library.empty() ? nullptr : FindLibrary(*f.store, o.ea.library);
                    float               nx = x;
                    if (l) {
                        dl->AddCircleFilled(ImVec2(x + ui::Dp(4.0f), y + ImGui::GetTextLineHeight() * 0.5f), ui::Dp(4.0f), ui::LibDot(l->color));
                        nx += ui::Dp(14.0f);
                    }
                    const std::string name = l ? l->name : o.ea.none ? std::string("None") : std::string("nothing");
                    ui::EllipsisTextTip(dl, ImVec2(nx, y), right, l ? ui::kText : ui::kFaint, name + (o.ea.level == AssignLevel::Unset ? std::string() : "  (" + o.ea.from + ")"));
                });
            }
        }
    }
}

void DrawDetails(const Frame& f)
{
    if (f.keyed && !g.sel.keys.empty()) {
        if (g.sel.keys[0].compare(0, 4, "kit:") == 0) {
            DrawKitDetails(f);
            return;
        }
        if (g.sel.keys[0].compare(0, 5, "char:") == 0) {
            DrawCharDetails(f);
            return;
        }
    }
    std::vector<const av::Target*> sel = SelectedTargets(f);
    if (sel.empty()) {
        ImGui::Dummy(ImVec2(0.0f, ui::Dp(16.0f)));
        ui::ToneText("Select a row on the left.", ui::kMuted, true);
        ui::ToneText("Shift-click picks a range, Ctrl-click adds one.", ui::kMuted, true);
        return;
    }
    const bool item_row = sel[0]->is_item;
    const av::Mix mix = item_row ? av::Mix() : av::MixOf(sel);
    // Title line.
    {
        ui::FontScope fs(ui::FontRole::PaneTitle);
        const std::string title = av::SelectionTitle(f.tg, sel);
        ui::ToneText(title, ui::kText, true);
    }
    if (mix.lib || mix.groups) {
        ImGui::SameLine();
        ui::Tag("mixed", ui::TagTone::Warn);
    }
    bool any_rule = false;
    for (const av::Target* t : sel) {
        int b = -1;
        if (av::BlockOfKey(t->key, &b) && b >= 0 && static_cast<size_t>(b) < f.blocks.size() && !IsAutoBlock(f.blocks[static_cast<size_t>(b)])) any_rule = true;
    }
    if (f.keyed) {
        ui::ToneText(f.key.scope_ch ? "every item of " + f.char_name + " (rig " + av::RigSig(f.key.scope_ch->skeleton) +
                                          "), unless the item sets its own"
                                    : std::string("project default \xC2\xB7 every character and item, unless they set their own"),
                     ui::kMuted, true);
    } else {
        ui::ToneText(av::ScopeLine(item_row, any_rule, f.pool_text, f.char_name), ui::kMuted, true);
    }

    if (!item_row) {
        DrawLibraryRow(f, sel, mix);
        if (f.keyed) DrawKeyExtras(f, sel);
    }
    if (mix.lib) {
        Banner(false,
               "These targets use " + std::to_string(std::set<std::string>([&] {
                   std::set<std::string> libs;
                   for (const av::Target* t : sel) libs.insert(t->ea.none ? "none" : t->ea.library);
                   return libs;
               }()).size()) + " different libraries. Pick one library above to edit their Switch values together.",
               nullptr);
        DrawPlays(sel, f.keyed);
        return;
    }
    const av::Target& first = *sel[0];
    if (!item_row && !first.lib) {
        ui::ToneText((first.Unset() ? std::string("Nothing set: no level above gives a library. ") : std::string("No sound. ")) +
                         "Populate places nothing for " + (sel.size() > 1 ? "these targets." : "this target."),
                     ui::kMuted, true);
        return;
    }
    if (!item_row && first.lib_empty) {
        Banner(true, first.lib->name + " is empty: it has no sound yet. Build it in the Library view; nothing is placed until then.", &first);
        return;
    }
    if (!item_row && first.lib_blocked)
        Banner(true, first.lib->name + " is blocked: " + first.blocked_reason + ". Nothing is placed from this library until it is fixed.", &first);

    // Switch values.
    std::vector<std::string> groups;
    for (const av::GroupState& gs : (item_row ? f.item.groups : first.groups)) groups.push_back(gs.group_uid);
    ui::Section     s;
    ui::SectionOpts o;
    o.suffix = item_row ? "all libraries of this item" : "in path order";
    if (ui::BeginSection(s, "Switch values", o)) {
        if (groups.empty()) ui::SubText("No library here switches on a group.");
        for (size_t i = 0; i < groups.size(); ++i) DrawGroupRow(f, sel, item_row, groups[i], static_cast<int>(i) + 1, i == 0);
    }
    ui::EndSection(s);
    if (item_row) return;
    DrawPlays(sel, f.keyed);
    if (sel.size() == 1) DrawLadder(f, first);
    if (f.keyed) return;
    // Where the sounds land in the Library.
    std::vector<std::string> tracks;
    for (const av::Target* t : sel)
        if (t->Plays()) {
            const std::string name = av::TrackNameOf(*t);
            if (std::find(tracks.begin(), tracks.end(), name) == tracks.end()) tracks.push_back(name);
        }
    if (!tracks.empty()) {
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::BeginGroup();
        ImGui::AlignTextToFramePadding();
        ui::ToneText("Lands in", ui::kMuted);
        ImGui::SameLine();
        ui::ToneText("RAV Library \xE2\x80\xBA " + (tracks.size() == 1 ? tracks[0] : std::to_string(tracks.size()) + " tracks"), ui::kText, true);
        ui::ToneText("set per sound in the Library view", ui::kFaint, true);
        ImGui::EndGroup();
        ui::DashedRect(ImGui::GetWindowDrawList(), ImVec2(p0.x - ui::Dp(4.0f), p0.y - ui::Dp(3.0f)),
                       ImVec2(ImGui::GetItemRectMax().x + ui::Dp(4.0f), ImGui::GetItemRectMax().y + ui::Dp(3.0f)), ui::kStrokeStrong,
                       ui::kRadiusMd);
    }
}

// ---- Overview -----------------------------------------------------------------------------------------------

// Moves to a scope (13-15c), keeping each scope's selection: the Project and Character scopes come back as they
// were (the first time on "Feet"), the Item scope starts empty.
void SwitchScope(int sc)
{
    if (sc == g.scope) return;
    g.sel_by[g.scope] = g.sel;
    g.scope = sc;
    g.nav.clear();
    if (sc == kScopeItem) {
        g.sel.Clear();
        g.sel_sig.clear();
        return;
    }
    g.sel = g.sel_by[sc];
    if (!g.key_init[sc]) {
        g.key_init[sc] = true;
        g.sel.keys.assign(1, "feet");
        g.sel.anchor = g.sel.cursor = "feet";
        g.closed["p:feet"] = false;
    }
}

// "Who plays what" (the Overview at Project / Character scope): the body parts and their types by the Project
// and every character; click a character's cell to edit that character, anything else to edit the project.
void DrawWhoOverview(const Frame& f, float height)
{
    if (!ui::BeginPane("##assignmx", ImVec2(0.0f, height), ui::kSurface, 0, ImGuiWindowFlags_HorizontalScrollbar)) {
        ui::EndPane();
        return;
    }
    const std::vector<av::CharEntry>& cs = f.key.chars;
    const size_t cols = 1 + cs.size();
    const float  avail = std::max(ui::Dp(420.0f), ImGui::GetContentRegionAvail().x);
    const float  c0 = ui::Dp(150.0f);
    const float  cw = std::max(ui::Dp(150.0f), (avail - c0) / static_cast<float>(cols));
    const float  total = c0 + cw * static_cast<float>(cols);
    const float  rh = ui::Dp(28.0f);
    const int    cur_col = g.scope == kScopeProject || f.key.cur < 0 ? 0 : 1 + f.key.cur;
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    {  // header
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ui::FontScope fs(ui::FontRole::Tiny);
        auto head = [&](float x, std::string t, bool cur) {
            for (char& c : t) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            ui::EllipsisTextTip(dl, ImVec2(p.x + x + ui::Dp(8.0f), p.y + ui::Dp(5.0f)), p.x + x + cw - ui::Dp(6.0f), cur ? ui::kAccentText : ui::kFaint, t);
        };
        dl->AddText(ImVec2(p.x + ui::Dp(8.0f), p.y + ui::Dp(5.0f)), ui::kFaint, "BODY PART \xC2\xB7 TYPE");
        head(c0, "Project", cur_col == 0);
        for (size_t i = 0; i < cs.size(); ++i) head(c0 + cw * static_cast<float>(i + 1), cs[i].ch.name, cur_col == static_cast<int>(i + 1));
        dl->AddLine(ImVec2(p.x, p.y + ui::Dp(22.0f)), ImVec2(p.x + total, p.y + ui::Dp(22.0f)), ui::kStroke);
        ImGui::Dummy(ImVec2(total, ui::Dp(24.0f)));
    }
    for (const BodyPart& bp : BodyParts()) {
        std::vector<std::string> keys = {bp.id};
        for (const PartType& t : bp.types) keys.push_back(AssignKey(bp.id, t.word));
        for (const std::string& key : keys) {
            const bool   is_part = key == bp.id;
            std::string  part_id, type_id;
            SplitAssignKey(key, &part_id, &type_id);
            ImGui::PushID(key.c_str());
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float  ty = std::floor(p.y + (rh - ImGui::GetTextLineHeight()) * 0.5f);
            // The current column behind the cells.
            dl->AddRectFilled(ImVec2(p.x + c0 + cw * static_cast<float>(cur_col), p.y), ImVec2(p.x + c0 + cw * static_cast<float>(cur_col + 1), p.y + rh),
                              ui::kAccentFaint);
            if (ui::SelectRow("##mxrow", g.sel.Has(key), 0, rh, 0, ui::kRadiusSm, total)) {
                const float mx = ImGui::GetIO().MousePos.x - p.x - c0;
                const int   col = mx < 0.0f ? 0 : static_cast<int>(mx / cw);  // 0 = the Project, i = a character
                if (col > 0 && static_cast<size_t>(col) <= cs.size()) {
                    g.cur_skel = cs[static_cast<size_t>(col - 1)].ch.skeleton;
                    g.cur_model = cs[static_cast<size_t>(col - 1)].ch.model;
                    SwitchScope(kScopeCharacter);
                } else {
                    SwitchScope(kScopeProject);
                }
                g.key_init[g.scope] = true;
                g.sel.Clear();
                g.sel.keys.push_back(key);
                g.sel.anchor = g.sel.cursor = key;
                g.closed["p:" + part_id] = false;
                g.folded["parts"] = false;
                g.overview = false;
            }
            {
                ui::FontScope fs(is_part ? ui::FontRole::Semibold : ui::FontRole::Body);
                ui::EllipsisTextTip(dl, ImVec2(p.x + ui::Dp(is_part ? 8.0f : 22.0f), ty), p.x + c0 - ui::Dp(6.0f), is_part ? ui::kText : ui::kFaint,
                                    is_part ? bp.label : TypeLabel(bp.id, type_id));
            }
            for (size_t c = 0; c < cols; ++c) {
                const av::WhoCell cell = av::WhoCellFor(*f.store, c == 0 ? nullptr : &cs[c - 1].ch, key);
                float             x = p.x + c0 + cw * static_cast<float>(c) + ui::Dp(8.0f);
                const float       lim = p.x + c0 + cw * static_cast<float>(c + 1) - ui::Dp(6.0f);
                const SoundLibrary* l = cell.library.empty() ? nullptr : FindLibrary(*f.store, cell.library);
                if (l) {
                    dl->AddCircleFilled(ImVec2(x + ui::Dp(4.0f), ty + ImGui::GetTextLineHeight() * 0.5f), ui::Dp(4.0f), ui::LibDot(l->color));
                    x += ui::Dp(14.0f);
                }
                std::string text = l ? l->name : cell.unset ? std::string("\xE2\x80\x94") : cell.none ? std::string("None") : std::string("(gone)");
                const float kit_w = cell.kit ? ui::SmallTagWidth("kit") + ui::Dp(6.0f) : 0.0f;
                if (!cell.own && !cell.unset) text += "  " + cell.from;
                ui::EllipsisTextTip(dl, ImVec2(x, ty), lim - kit_w, cell.own ? ui::kText : cell.unset ? ui::kFaint : ui::kMuted, text);
                if (cell.kit && lim - kit_w > x + ui::Dp(40.0f)) {
                    const float tw = std::min(ImGui::CalcTextSize(text.c_str()).x, lim - kit_w - x);
                    ui::DrawSmallTag(dl, ImVec2(x + tw + ui::Dp(6.0f), p.y + (rh - ui::SmallTagHeight()) * 0.5f), "kit");
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rh));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            ImGui::PopID();
        }
    }
    ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
    ui::ToneText("Who plays what, per character. White = set at that level; grey = inherited (its source after it). Click a character's cell "
                 "to edit that character, a row to edit the project.",
                 ui::kMuted, true);
    ui::EndPane();
}

void DrawOverview(const Frame& f, float height)
{
    if (f.keyed) {
        DrawWhoOverview(f, height);
        return;
    }
    if (!ui::BeginPane("##assignmx", ImVec2(0.0f, height), ui::kSurface, 0, ImGuiWindowFlags_HorizontalScrollbar)) {
        ui::EndPane();
        return;
    }
    const std::vector<std::string> groups = av::OverviewGroups(*f.store, f.targets);
    const float  avail = std::max(ui::Dp(420.0f), ImGui::GetContentRegionAvail().x);
    const float  c0 = ui::Dp(150.0f), c1 = ui::Dp(150.0f);
    const float  cg = groups.empty() ? 0.0f : std::max(ui::Dp(84.0f), (avail - c0 - c1) / static_cast<float>(groups.size()));
    const float  total = c0 + c1 + cg * static_cast<float>(groups.size());
    const float  rh = ui::Dp(28.0f);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    {  // header
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ui::FontScope fs(ui::FontRole::Tiny);
        auto head = [&](float x, const std::string& t) {
            std::string up = t;
            for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            dl->AddText(ImVec2(p.x + x + ui::Dp(8.0f), p.y + ui::Dp(5.0f)), ui::kFaint, up.c_str());
        };
        head(0.0f, "Target");
        head(c0, "Library");
        for (size_t i = 0; i < groups.size(); ++i) {
            const SwitchGroup* g2 = FindGroup(*f.store, groups[i]);
            head(c0 + c1 + cg * static_cast<float>(i), g2 ? g2->name : std::string("?"));
        }
        dl->AddLine(ImVec2(p.x, p.y + ui::Dp(22.0f)), ImVec2(p.x + total, p.y + ui::Dp(22.0f)), ui::kStroke);
        ImGui::Dummy(ImVec2(total, ui::Dp(24.0f)));
    }
    auto block_rows = [&](const char* label, const std::vector<av::Parent>& parents, const std::string& scope) {
        if (parents.empty()) return;
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(p, ImVec2(p.x + total, p.y + ui::Dp(24.0f)), ui::kBg);
            ui::FontScope fs(ui::FontRole::Small);
            dl->AddText(ImVec2(p.x + ui::Dp(8.0f), p.y + ui::Dp(5.0f)), ui::kMuted, (std::string(label) + kDot + scope).c_str());
            ImGui::Dummy(ImVec2(total, ui::Dp(24.0f)));
        }
        for (const av::Parent& par : parents)
            for (const av::Leaf& l : par.leaves) {
                const av::Target& t = f.targets[static_cast<size_t>(l.block)];
                ImGui::PushID(l.key.c_str());
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float  ty = std::floor(p.y + (rh - ImGui::GetTextLineHeight()) * 0.5f);
                if (ui::SelectRow("##mxrow", g.sel.Has(l.key), 0, rh, 0, ui::kRadiusSm, total)) {
                    g.sel.Clear();
                    g.sel.keys.push_back(l.key);
                    g.sel.anchor = g.sel.cursor = l.key;
                    g.closed[par.id] = false;
                    g.folded[par.is_auto ? "auto" : "rules"] = false;
                    g.overview = false;
                }
                ui::EllipsisTextTip(dl, ImVec2(p.x + ui::Dp(8.0f), ty), p.x + c0 - ui::Dp(6.0f), ui::kText, LeafTitle(par, l));
                {  // the library cell: dot, name, where it comes from
                    float x = p.x + c0 + ui::Dp(8.0f);
                    if (t.lib) {
                        dl->AddCircleFilled(ImVec2(x + ui::Dp(4.0f), ty + ImGui::GetTextLineHeight() * 0.5f), ui::Dp(4.0f), ui::LibDot(t.lib->color));
                        x += ui::Dp(14.0f);
                    }
                    std::string text = t.lib ? t.lib->name : (t.Unset() ? std::string("\xE2\x80\x94") : std::string("None"));
                    if (t.lib_empty) text += "  [empty]";
                    else if (t.lib_blocked) text += "  [blocked]";
                    if (!t.OwnLib() && !t.Unset()) text += "  " + t.ea.from;
                    ui::EllipsisTextTip(dl, ImVec2(x, ty), p.x + c0 + c1 - ui::Dp(6.0f), t.OwnLib() ? ui::kText : ui::kMuted, text);
                }
                for (size_t i = 0; i < groups.size(); ++i) {
                    const av::Cell c = av::OverviewCell(t, groups[i]);
                    const float    x = p.x + c0 + c1 + cg * static_cast<float>(i) + ui::Dp(8.0f);
                    const char*    txt = c.kind == av::CellKind::Value ? c.text.c_str() : c.kind == av::CellKind::NotReached ? "not reached" : "\xE2\x80\x94";
                    const ImU32    col = c.kind == av::CellKind::Value ? (c.inherited ? ui::kMuted : ui::kText) : ui::kFaint;
                    ui::EllipsisTextTip(dl, ImVec2(x, ty), x + cg - ui::Dp(12.0f), col, txt);
                }
                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rh));
                ImGui::Dummy(ImVec2(0.0f, 0.0f));
                ImGui::PopID();
            }
    };
    block_rows("Bone rules", f.tg.rules, f.pool_text);
    block_rows("Auto detection", f.tg.autos, "this item");
    ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
    ui::ToneText("Grey = inherited (from the character, the project, the item or the Switch default). Click a row to edit it in the tree.",
                 ui::kMuted, true);
    ui::EndPane();
}

// ---- The window --------------------------------------------------------------------------------------------

void DrawTitleRow(const Frame& f)
{
    const float  h = ui::Dp(28.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float  close_w = ui::ControlH();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    // Antho 2026-10-12: the window is a window of its own, anywhere on the screen: its title row moves it
    // (the OS drag, FloatUiWindow's caption area), unless a menu is open over it.
    g_host.SetDragArea(p.x - ui::Dp(8.0f), p.y - ui::Dp(8.0f), p.x + w - close_w - ui::Dp(4.0f), p.y + h,
                       ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
    const float ty = std::floor(p.y + (h - ImGui::GetTextLineHeight()) * 0.5f);
    std::string title = "Sound assignment";
    const float tw = ImGui::CalcTextSize(title.c_str()).x;
    dl->AddText(ImVec2(p.x + ui::Dp(4.0f), ty), ui::kText, title.c_str());
    if (!f.item_name.empty()) {
        const float x = p.x + ui::Dp(4.0f) + tw + ui::Dp(6.0f);
        ui::EllipsisTextTip(dl, ImVec2(x, ty), p.x + w - close_w - ui::Dp(8.0f), ui::kMuted, std::string("\xC2\xB7 ") + f.item_name);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - close_w, p.y + (h - close_w) * 0.5f));
    if (ui::IconOnlyButton("##assignclose", ToolIcon(kToolCross), "Close (Esc)")) g.open = false;
    dl->AddLine(ImVec2(p.x - ui::Dp(8.0f), p.y + h), ImVec2(p.x + w + ui::Dp(8.0f), p.y + h), ui::kStroke);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + ui::Dp(4.0f)));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

// "Reads: Kit > Project > Hero > Hero_Run_01 > rule > event": the level being edited lit, the ones beyond it off.
void DrawCrumb(const Frame& f)
{
    struct Lv {
        std::string t;
        bool        cur, off;
    };
    const std::string ch = f.char_name.empty() ? std::string("Character") : f.char_name;
    const std::vector<Lv> lv = {{"Kit", false, false},
                                {"Project", g.scope == kScopeProject, false},
                                {g.scope == kScopeProject ? std::string("Character") : ch, g.scope == kScopeCharacter, g.scope == kScopeProject},
                                {g.scope == kScopeItem && !f.item_name.empty() ? f.item_name : std::string("Item"), g.scope == kScopeItem,
                                 g.scope != kScopeItem},
                                {"rule", false, g.scope != kScopeItem},
                                {"event", false, g.scope != kScopeItem}};
    ui::FontScope fs(ui::FontRole::Small);
    ImDrawList*   dl = ImGui::GetWindowDrawList();
    const ImVec2  p0 = ImGui::GetCursorScreenPos();
    const float   avail = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float   lh = ImGui::GetTextLineHeight() + ui::Dp(3.0f);
    float         x = 0.0f, y = 0.0f;
    auto put = [&](const std::string& t, ImU32 col, bool strike) {
        const float w = ImGui::CalcTextSize(t.c_str()).x;
        if (x > 0.0f && x + w > avail) {
            x = 0.0f;
            y += lh;
        }
        dl->AddText(ImVec2(p0.x + x, p0.y + y), col, t.c_str());
        if (strike)
            dl->AddLine(ImVec2(p0.x + x, p0.y + y + ImGui::GetTextLineHeight() * 0.55f),
                        ImVec2(p0.x + x + w, p0.y + y + ImGui::GetTextLineHeight() * 0.55f), ui::kStrokeStrong);
        x += w;
    };
    put("Reads:", ui::kFaint, false);
    x += ui::Dp(5.0f);
    for (size_t i = 0; i < lv.size(); ++i) {
        if (i > 0) {
            put("\xE2\x80\xBA", ui::kFaint, false);
            x += ui::Dp(5.0f);
        }
        put(lv[i].t, lv[i].cur ? ui::kAccentText : lv[i].off ? ui::kFaint : ui::kMuted, lv[i].off);
        x += ui::Dp(5.0f);
    }
    ImGui::Dummy(ImVec2(avail, y + lh));
}

void DrawItemPicker(const Frame& f)
{
    std::string preview = f.item_name.empty() ? std::string("No animation item") : f.item_name;
    if (!f.char_name.empty()) preview += kDot + f.char_name;
    if (!ui::BeginComboStyled("##assignitem", preview.c_str(), ui::Dp(210.0f))) return;
    // Review 13-15b: the list is read once each time it opens (each item's rig may parse its file).
    static std::vector<std::pair<MediaItem*, std::string>> list;
    if (ImGui::IsWindowAppearing()) {
        list.clear();
        for (MediaItem* it : ProjectRavItems()) {
            std::string label = ItemNameOf(it);
            if (label.empty()) label = "(unnamed item)";
            const ItemRigInfo rig = ItemRig(it);
            if (const SoundCharacter* c = CharacterFor(GetSoundStore(nullptr), rig.skeleton, RigModel(rig)))
                label += kDot + c->name;
            list.push_back({it, label});
        }
    }
    const std::vector<std::pair<MediaItem*, std::string>>& items = list;
    int                                                   i = 0;
    for (const auto& entry : items) {
        MediaItem*         it = entry.first;
        const std::string& label = entry.second;
        const bool cur = it == g.item;
        if (ImGui::Selectable((label + "##item" + std::to_string(i++)).c_str(), cur) && !cur) {
            g.item = it;  // window-local: the sound store is untouched
            g.follow = false;
        }
    }
    if (items.empty()) ImGui::TextUnformatted("No animation item in the project");
    ui::EndComboStyled();
}

// The Character scope's picker: one entry per character of the project; the one picked is the current one.
void DrawCharPicker(const Frame& f)
{
    std::string preview = "No character yet";
    if (f.key.cur >= 0) {
        const av::CharEntry& c = f.key.chars[static_cast<size_t>(f.key.cur)];
        preview = c.ch.name + kDot + "rig " + av::RigSig(c.ch.skeleton);
    }
    if (!ui::BeginComboStyled("##assignchar", preview.c_str(), ui::Dp(210.0f))) return;
    for (size_t i = 0; i < f.key.chars.size(); ++i) {
        const av::CharEntry& c = f.key.chars[i];
        std::string          label = c.ch.name + kDot + "rig " + av::RigSig(c.ch.skeleton);
        if (!c.ch.model.empty() && !c.models.empty() && !c.models[0].files.empty()) label += kDot + c.models[0].files.front();
        const bool cur = static_cast<int>(i) == f.key.cur;
        if (ImGui::Selectable((label + "##" + c.key).c_str(), cur) && !cur) {
            g.cur_skel = c.ch.skeleton;
            g.cur_model = c.ch.model;
            if (!g.sel.keys.empty() && g.sel.keys[0].compare(0, 5, "char:") == 0) {
                g.sel.keys.assign(1, c.key);
                g.sel.anchor = g.sel.cursor = c.key;
            }
        }
    }
    ui::EndComboStyled();
}

// Drops the selected rows that went away (a kit removed, a character gone); a character stored since keeps its row.
void PruneKeySel(const Frame& f)
{
    std::vector<std::string> valid = av::AllKeys();
    for (const SoundLibrary& l : f.store->libraries)
        if (!l.kit.empty()) valid.push_back("kit:" + l.uid);
    for (const av::CharEntry& c : f.key.chars) valid.push_back(c.key);
    if (!g.sel.keys.empty() && av::KindOf(g.sel.keys[0]) == av::RowKind::Char &&
        std::find(valid.begin(), valid.end(), g.sel.keys[0]) == valid.end() && f.key.cur >= 0) {
        g.sel.keys.assign(1, f.key.chars[static_cast<size_t>(f.key.cur)].key);
        g.sel.anchor = g.sel.cursor = g.sel.keys[0];
    }
    av::Prune(g.sel, valid);
}

void DrawFooter()
{
    const float  h = ImGui::GetTextLineHeight() + ui::Dp(10.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    ui::FontScope fs(ui::FontRole::Small);
    dl->AddLine(ImVec2(p.x - ui::Dp(8.0f), p.y), ImVec2(p.x + w + ui::Dp(8.0f), p.y), ui::kStroke);
    const float ty = std::floor(p.y + (h - ImGui::GetTextLineHeight()) * 0.5f);
    const char  right[] = "Each change is one undo step.";
    const float rw = ImGui::CalcTextSize(right).x;
    const bool  show_right = w > rw + ui::Dp(220.0f);
    if (show_right) dl->AddText(ImVec2(p.x + w - rw - ui::Dp(4.0f), ty), ui::kMuted, right);
    const float lim = p.x + w - (show_right ? rw + ui::Dp(16.0f) : 0.0f);
    float       x = p.x + ui::Dp(4.0f);
    if (g.has_last) {
        dl->AddText(ImVec2(x, ty), ui::kMuted, g.last.prefix.c_str());
        x += ImGui::CalcTextSize(g.last.prefix.c_str()).x;
        ui::AddTextIn(dl, ui::FontCaptionBold(), ImVec2(x, ty), ui::kText, g.last.bold.c_str());
        x += ui::TextSizeIn(ui::FontCaptionBold(), g.last.bold.c_str()).x;
        ui::EllipsisTextTip(dl, ImVec2(x, ty), lim, ui::kMuted, g.last.rest);
    } else {
        ui::EllipsisTextTip(dl, ImVec2(x, ty), lim, ui::kMuted, g.last_note.empty() ? std::string("No change yet.") : g.last_note);
    }
    ImGui::Dummy(ImVec2(w, h));
}

void DrawTreePane(const Frame& f, ImVec2 size)
{
    if (ui::BeginPane("##assigntree", size, ui::kSurface)) {
        const std::vector<Row> rows = f.keyed ? BuildKeyRows(f) : BuildRows(f);
        if (rows.empty()) ui::SubText("Nothing matches the filter.");
        DrawTreeRows(f, rows);
        HandleTreeKeys(rows);
    }
    ui::EndPane();
}

void DrawTreeAndDetails(const Frame& f, float body_h)
{
    const float total = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    if (total < ui::Dp(640.0f)) {  // a narrow window: the details go under the tree
        const float th = std::max(ui::Dp(60.0f), body_h * 0.45f);
        DrawTreePane(f, ImVec2(0.0f, th));
        if (ui::BeginPane("##assigndet", ImVec2(0.0f, std::max(ui::Dp(40.0f), body_h - th - ImGui::GetStyle().ItemSpacing.y)), ui::kSurface))
            DrawDetails(f);
        ui::EndPane();
        return;
    }
    if (g.split < 0.0f) g.split = total * 0.4f;
    const float hit = ui::Dp(8.0f);
    g.split = std::max(ui::Dp(240.0f), std::min(g.split, std::max(ui::Dp(240.0f), total - ui::Dp(260.0f) - hit)));
    DrawTreePane(f, ImVec2(g.split, body_h));
    ImGui::SameLine(0.0f, 0.0f);
    const ui::SplitterState sp = ui::SplitterBar("##assignsplit", ImVec2(hit, body_h));
    if (sp.active) g.split += sp.delta;
    if (sp.double_clicked) g.split = total * 0.4f;
    ImGui::SameLine(0.0f, 0.0f);
    if (ui::BeginPane("##assigndet", ImVec2(0.0f, body_h), ui::kSurface)) DrawDetails(f);
    ui::EndPane();
}

// Applies a request from the strip: select those rules' leaves, unfold their section.
void ApplyRequest(const Frame& f)
{
    g.request = false;
    g.scope = kScopeItem;
    g.overview = false;
    g.filter[0] = '\0';
    g.sel.Clear();
    for (int b : g.request_blocks) {
        const std::string key = av::LeafKey(b);
        const av::Parent* p = f.tg.ParentOf(key);
        if (!p) continue;
        g.sel.keys.push_back(key);
        g.closed[p->id] = false;
        g.folded[p->is_auto ? "auto" : "rules"] = false;
        if (g.sel.anchor.empty()) {
            g.sel.anchor = g.sel.cursor = key;
            g.nav = "P:" + p->id;
        }
    }
    g.request_blocks.clear();
    g.nav_shown = true;
    g.nav_scroll = true;
}

}  // namespace

void OpenAssignWindow(MediaItem* item, const std::vector<int>& blocks)
{
    g.open = true;
    g.rigs_count = -1;  // the rigs of the project are read again
    g.focus_request = true;
    if (item && !blocks.empty()) {
        g.request = true;
        g.request_item = item;
        g.request_blocks = blocks;
    }
}

void CloseAssignWindow()
{
    g.open = false;
}

bool AssignWindowIsOpen()
{
    return g.open;
}

bool TaggingAssignTakesKey(unsigned vk, bool ctrl, bool shift, bool alt)
{
    (void)shift;
    if (ctrl || alt || !TaggingAssignWindowOpen()) return false;
    constexpr unsigned kBack = 0x08, kEnter = 0x0D, kEscape = 0x1B, kLeft = 0x25, kUp = 0x26, kRight = 0x27, kDown = 0x28,
                       kDelete = 0x2E;
    return vk == kBack || vk == kEnter || vk == kEscape || vk == kLeft || vk == kUp || vk == kRight || vk == kDown ||
           vk == kDelete;
}

bool TaggingAssignWindowOpen()
{
    return g.open && g_host.HasFocus();
}

void DrawAssignWindow(const TaggingModel& m)
{
    if (!g.open) return;
    if (m.item != g.last_model_item) {  // the Tagging view moved to another item: the window follows it again
        g.last_model_item = m.item;
        g.follow = true;
    }
    if (g.request) {
        g.item = g.request_item;
        g.follow = g.request_item == m.item;
    } else if (g.follow) {
        g.item = m.item;
    }
}

namespace {

// The window's content, in its own window's ImGui context (FloatUiWindow::Frame: one window covers it all).
void DrawAssignContent()
{
    {
        const int  frame = ImGui::GetFrameCount();
        const bool focused = g_host.HasFocus();
        // Another project tab while typing a name: the typing is dropped (never renames the other project's).
        if (g.name_active && EnumProjects(-1, nullptr, 0) != g.name_proj) g.name_active = false;
        // A click outside the Name field ends its typing before the click changes the selection.
        if (g.name_active && ImGui::IsMouseClicked(0) && !ImGui::IsMouseHoveringRect(g.name_min, g.name_max, false))
            CommitName();
        if (g.request && g.scope != kScopeItem) SwitchScope(kScopeItem);  // the strip's Assign... opens the Item scope
        Frame f;
        bool  ok = false;
        if (g.scope != kScopeItem) {
            BuildKeyFrame(&f);
            ok = f.ok;
            PruneKeySel(f);
        } else {
            ok = BuildFrame(g.item, &f);
        }
        if (g.scope != kScopeItem) {
            // (the Project and Character scopes keep their selection until a row goes away)
        } else if (ok) {
            if (g.sel_item != g.item) {  // another item: its rows are not these rows
                g.sel.Clear();
                g.closed.clear();
                g.nav.clear();
                g.sel_item = g.item;
            }
            if (g.sel_sig != f.tg.signature) {
                g.sel_sig = f.tg.signature;
                if (!g.request) g.sel.Clear();
            }
            if (g.request) ApplyRequest(f);
        } else if (g.request) {
            g.request = false;  // its item is gone or unreadable: follow the Tagging item again
            g.follow = true;
        }
        if (focused && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput) {
            // Esc in an open menu closes the menu only (no keyboard nav here, so ImGui does not do it).
            if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) ImGui::ClosePopupToLevel(0, true);
            else g.open = false;
        }

        DrawTitleRow(f);
        {  // Scope bar: where you edit, the item, the reading order.
            static const char* const kScopes[] = {"Project", "Character", "Item"};
            int                      sc = g.scope;
            if (ui::Segmented("##assignscope", kScopes, 3, &sc)) SwitchScope(sc);
            if (g.scope == kScopeItem) {
                ui::SameLineIfFits(ui::Dp(210.0f));
                DrawItemPicker(f);
            } else if (g.scope == kScopeCharacter) {
                ui::SameLineIfFits(ui::Dp(210.0f));
                DrawCharPicker(f);
            }
            DrawCrumb(f);
        }
        ImGui::Separator();
        {  // View, filter, Library view
            static const char* const kModes[] = {"Tree", "Overview"};
            int                      md = g.overview ? 1 : 0;
            if (ui::Segmented("##assignmode", kModes, 2, &md)) g.overview = md == 1;
            ui::SameLineIfFits(ui::Dp(150.0f));
            const char* hint = g.scope == kScopeItem      ? "Filter rules and types"
                               : g.scope == kScopeProject ? "Filter kits and body parts"
                                                          : "Filter characters and body parts";
            const float bw = ui::ButtonWidth("Library view \xE2\x86\x97##assignlib");
            const float fw = std::max(ui::Dp(100.0f), std::min(ui::Dp(240.0f), ImGui::GetContentRegionAvail().x - bw - ImGui::GetStyle().ItemSpacing.x));
            ui::TextField("##assignfilter", hint, g.filter, sizeof(g.filter), fw);
            if (ui::SameLineIfFits(bw)) {
                const float room = ImGui::GetContentRegionAvail().x - bw;
                if (room > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room);
            }
            if (ui::SolidButton("Library view \xE2\x86\x97##assignlib")) {
                const std::vector<const av::Target*> sel = ok ? SelectedTargets(f) : std::vector<const av::Target*>();
                std::set<std::string>                libs;
                for (const av::Target* t : sel)
                    if (t->lib) libs.insert(t->lib->uid);
                if (ok && f.keyed)
                    for (const std::string& k : g.sel.keys)
                        if (k.compare(0, 4, "kit:") == 0) libs.insert(k.substr(4));  // a kit row: its library
                if (libs.size() == 1) OpenLibraryViewUid(*libs.begin());
                else SetViewMode(ViewMode::Library);
            }
            ui::ItemTipText("Open the Library view (this window stays open)");
        }
        const float foot_h = ImGui::GetTextLineHeight() + ui::Dp(10.0f) + ImGui::GetStyle().ItemSpacing.y;
        const float body_h = std::max(ui::Dp(80.0f), ImGui::GetContentRegionAvail().y - foot_h);
        if (!ok) {
            ui::SubText(g.scope == kScopeItem
                            ? "No animation item with readable rules is shown. Select a RAV animation item in REAPER."
                            : "No animation item in the project has a readable rig yet: its character appears here.");
            ImGui::Dummy(ImVec2(0.0f, body_h - ImGui::GetTextLineHeightWithSpacing()));
        } else if (g.overview) {
            DrawOverview(f, body_h);
        } else {
            DrawTreeAndDetails(f, body_h);
        }
        DrawFooter();
        if (g.name_active && g.name_frame != frame) CommitName();  // the sheet is gone (scope, selection): kept
    }
}

}  // namespace

void DrawAssignFloat(HWND viewer, HDC viewer_dc, HGLRC rc)
{
    if (!ImGui::GetCurrentContext()) return;
    if (!g.open) CommitName();  // closed while typing a name: it is kept
    g_host.Bind(viewer, viewer_dc, rc);
    const bool activate = g.open && g.focus_request;
    g.focus_request = false;
    g_host.Frame(g.open, activate, [] { DrawAssignContent(); });
    if (g_host.TakeCloseRequest()) g.open = false;
    // Its gestures run now, in any view (the Tagging view runs the strip's own after its widgets).
    RunPickedSoundGestures();
}

void HideAssignFloat()
{
    g_host.Hide();
}

void DestroyAssignFloat()
{
    g_host.Destroy();
    FloatUiWindow::UnregisterWindowClass();  // a later load of the DLL registers its own
}

bool AssignFloatOwns(HWND hwnd)
{
    const HWND h = g_host.Hwnd();
    return h && hwnd && (hwnd == h || IsChild(h, hwnd));
}

bool AssignFloatWantsText()
{
    return g_host.WantsText();
}

std::vector<int> RuleGroupBlocks(const std::vector<Block>& blocks, int b)
{
    std::vector<int> out;
    if (b < 0 || static_cast<size_t>(b) >= blocks.size()) return out;
    const av::Targets tg = av::BuildTargets(blocks);
    if (const av::Parent* p = tg.ParentOf(av::LeafKey(b))) {
        for (const av::Leaf& l : p->leaves) out.push_back(l.block);
    } else {
        out.push_back(b);
    }
    return out;
}

void DrawAssignSummaryRow(const char* id, MediaItem* item, const std::vector<Block>& blocks, const std::vector<int>& open_blocks,
                          const char* label)
{
    if (!item || blocks.empty()) return;
    RefreshCache(item, g_strip_cache);
    const SoundStore&     store = GetSoundStore(nullptr);
    const ItemRigInfo&    rig = g_strip_cache.rig;
    const SoundCharacter* ch = CharacterFor(store, rig.skeleton, RigModel(rig));
    std::vector<av::Target> ts;
    for (const Block& b : blocks) ts.push_back(av::MakeTarget(store, g_strip_cache.rec, ch, b, std::string(), std::string()));
    std::vector<const av::Target*> ptrs;
    for (const av::Target& t : ts) ptrs.push_back(&t);
    const av::Summary sum = av::Summarize(store, ptrs, true);

    ImGui::PushID(id);
    const bool   can = !open_blocks.empty();
    const char*  btn = "Assign...##assign";
    const float  bw = ui::ButtonWidth(btn);
    const float  sp = ImGui::GetStyle().ItemSpacing.x;
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float  lw = label && label[0] ? ImGui::CalcTextSize(label).x + ui::Dp(6.0f) : 0.0f;
    const bool   narrow = w < lw + ui::Dp(120.0f) + bw + sp;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  fh = ImGui::GetFrameHeight();
    const float  ty = std::floor(p.y + (fh - ImGui::GetTextLineHeight()) * 0.5f);
    if (lw > 0.0f) dl->AddText(ImVec2(p.x, ty), ui::kMuted, label);
    // Review 13-15b: an empty or blocked library is flagged here too, as in the window's tree.
    const float warn_w = sum.blocked ? ImGui::CalcTextSize("!").x + ui::Dp(6.0f) : 0.0f;
    if (sum.blocked) ui::AddTextIn(dl, ui::FontSemibold(), ImVec2(p.x + lw, ty), ui::kWarn, "!");
    const float sum_w = std::max(ui::Dp(40.0f), (narrow ? w : w - bw - sp) - lw - warn_w);
    const bool  cut = PaintSummary(dl, p.x + lw + warn_w, ty, sum_w, sum, false);
    ImGui::Dummy(ImVec2(sum_w + lw + warn_w, fh));
    if (cut || sum.blocked)
        ui::ItemTipText(sum.blocked ? (sum.Plain() + "\nNothing is placed: open Assign... for the details").c_str()
                                    : sum.Plain().c_str());
    if (!narrow) ImGui::SameLine(0.0f, sp);
    if (!can) ImGui::BeginDisabled();
    if (ui::SolidButton(btn) && can) OpenAssignWindow(item, open_blocks);
    if (!can) ImGui::EndDisabled();
    ui::ItemTipText(can ? "Open the Sound assignment window on this" : "Detect first, or set it at Project / Character level");
    ImGui::PopID();
}

}  // namespace rav

#endif  // _WIN32
