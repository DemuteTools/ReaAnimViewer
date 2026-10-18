// SPDX-License-Identifier: MIT
//
// See library_tree_edit.h.

#include "library_tree_edit.h"

#include <algorithm>
#include <cctype>

#include "rule_record.h"
#include "sound_record.h"
#include "sound_switch.h"

namespace rav {
namespace {

constexpr const char kUnassignedKey[] = "#unassigned";
constexpr const char kHintKey[] = "#hint";

bool IsContainer(const SoundNode& n) { return n.type != SoundNodeType::Sound; }

const char* TypeName(SoundNodeType t)
{
    switch (t) {
    case SoundNodeType::Sound:  return "New Sound";
    case SoundNodeType::Random: return "Random";
    case SoundNodeType::Blend:  return "Blend";
    default:                    return "Switch";
    }
}

int IndexOf(const SoundLibrary& lib, const std::string& uid)
{
    for (size_t i = 0; i < lib.nodes.size(); ++i)
        if (lib.nodes[i].uid == uid) return static_cast<int>(i);
    return -1;
}

SoundNode* ParentOf(SoundLibrary& lib, const std::string& uid)
{
    const SoundNode* n = FindNode(lib, uid);
    if (!n || n->parent.empty() || n->parent == uid) return nullptr;
    return FindNode(lib, n->parent);
}

// uid and every node under it (cycle-safe).
std::set<std::string> Subtree(const SoundLibrary& lib, const std::string& uid)
{
    std::set<std::string> out;
    if (!FindNode(lib, uid)) return out;
    out.insert(uid);
    bool grew = true;
    while (grew) {
        grew = false;
        for (const SoundNode& n : lib.nodes)
            if (!out.count(n.uid) && out.count(n.parent)) {
                out.insert(n.uid);
                grew = true;
            }
    }
    return out;
}

// Where a node sits under its parent, to put another node there.
struct Slot {
    std::string parent;       // "" = the root
    std::string value;        // a Switch case's value
    bool        unassigned = false;
    int         unassigned_idx = -1;
    std::string next_sibling; // the sibling after it (vector order), "" = last
};

Slot SlotOf(SoundLibrary& lib, const std::string& uid)
{
    Slot s;
    const SoundNode* n = FindNode(lib, uid);
    if (!n) return s;
    if (uid == lib.root) return s;
    s.parent = n->parent;
    SoundNode* p = ParentOf(lib, uid);
    if (p && p->type == SoundNodeType::Switch) {
        for (const SwitchCase& c : p->cases)
            if (c.child_uid == uid) s.value = c.value_uid;
        if (s.value.empty()) {
            for (size_t i = 0; i < p->unassigned.size(); ++i)
                if (p->unassigned[i] == uid) s.unassigned_idx = static_cast<int>(i);
            s.unassigned = true;
        }
    } else {
        bool seen = false;
        for (const SoundNode& k : lib.nodes) {
            if (k.parent != n->parent || k.uid == n->parent) continue;
            if (seen) {
                s.next_sibling = k.uid;
                break;
            }
            if (k.uid == uid) seen = true;
        }
    }
    return s;
}

// Takes the node off its parent Switch's case / Unassigned list (its `parent` is left as is).
void Detach(SoundLibrary& lib, const std::string& uid)
{
    for (SoundNode& p : lib.nodes) {
        if (p.type != SoundNodeType::Switch) continue;
        p.cases.erase(std::remove_if(p.cases.begin(), p.cases.end(),
                                     [&](const SwitchCase& c) { return c.child_uid == uid; }),
                      p.cases.end());
        p.unassigned.erase(std::remove(p.unassigned.begin(), p.unassigned.end(), uid), p.unassigned.end());
    }
}

// Moves the node's line just before `before` ("" = to the end).
void MoveLineBefore(SoundLibrary& lib, const std::string& uid, const std::string& before)
{
    int i = IndexOf(lib, uid);
    if (i < 0 || uid == before) return;
    SoundNode n = std::move(lib.nodes[static_cast<size_t>(i)]);
    lib.nodes.erase(lib.nodes.begin() + i);
    int j = before.empty() ? -1 : IndexOf(lib, before);
    if (j < 0) lib.nodes.push_back(std::move(n));
    else lib.nodes.insert(lib.nodes.begin() + j, std::move(n));
}

void MoveLineAfter(SoundLibrary& lib, const std::string& uid, const std::string& after)
{
    int i = IndexOf(lib, uid);
    if (i < 0 || uid == after) return;
    SoundNode n = std::move(lib.nodes[static_cast<size_t>(i)]);
    lib.nodes.erase(lib.nodes.begin() + i);
    int j = IndexOf(lib, after);
    if (j < 0) lib.nodes.push_back(std::move(n));
    else lib.nodes.insert(lib.nodes.begin() + j + 1, std::move(n));
}

// Puts a detached node in a slot.
void Attach(SoundLibrary& lib, const std::string& uid, const Slot& s)
{
    SoundNode* n = FindNode(lib, uid);
    if (!n) return;
    if (s.parent.empty()) {
        n->parent.clear();
        lib.root = uid;
        return;
    }
    n->parent = s.parent;
    SoundNode* p = FindNode(lib, s.parent);
    if (p && p->type == SoundNodeType::Switch) {
        if (!s.value.empty()) {
            bool set = false;
            for (SwitchCase& c : p->cases)
                if (c.value_uid == s.value) {
                    c.child_uid = uid;
                    set = true;
                }
            if (!set) {
                SwitchCase c;
                c.value_uid = s.value;
                c.child_uid = uid;
                p->cases.push_back(c);
            }
        } else {
            size_t at = s.unassigned_idx < 0 ? p->unassigned.size()
                                             : std::min(p->unassigned.size(), static_cast<size_t>(s.unassigned_idx));
            p->unassigned.insert(p->unassigned.begin() + static_cast<long>(at), uid);
        }
        return;
    }
    MoveLineBefore(lib, uid, s.next_sibling);
}

void EraseSubtree(SoundLibrary& lib, const std::string& uid)
{
    std::set<std::string> sub = Subtree(lib, uid);
    for (const std::string& u : sub) Detach(lib, u);
    lib.nodes.erase(std::remove_if(lib.nodes.begin(), lib.nodes.end(),
                                   [&](const SoundNode& n) { return sub.count(n.uid) != 0; }),
                    lib.nodes.end());
}

// Copies the subtree into `dst` (new uids); the copy's top keeps the original's parent (the
// caller places it). Returns the copy's uid.
std::string CopySubtree(const SoundLibrary& src, const std::string& uid, SoundLibrary& dst)
{
    std::set<std::string> sub = Subtree(src, uid);
    std::vector<SoundNode> copies;
    std::vector<std::pair<std::string, std::string>> map;
    for (const SoundNode& n : src.nodes)
        if (sub.count(n.uid)) map.emplace_back(n.uid, NewSoundUid());
    auto remap = [&](const std::string& u) {
        for (const auto& m : map)
            if (m.first == u) return m.second;
        return u;
    };
    for (const SoundNode& n : src.nodes) {
        if (!sub.count(n.uid)) continue;
        SoundNode c = n;
        c.uid = remap(n.uid);
        if (n.uid != uid) c.parent = remap(n.parent);
        for (SwitchCase& sc : c.cases) sc.child_uid = remap(sc.child_uid);
        for (std::string& u : c.unassigned) u = remap(u);
        copies.push_back(std::move(c));
    }
    std::string top = remap(uid);
    for (SoundNode& c : copies) dst.nodes.push_back(std::move(c));
    return top;
}

// The rows' nodes, once each, none under another of them; the root only when `allow_root`.
std::vector<std::string> RowNodes(const SoundLibrary& lib, const std::vector<LibraryRow>& rows, bool allow_root)
{
    std::vector<std::string> out;
    for (const LibraryRow& r : rows) {
        std::string n = RowNode(lib, r);
        if (n.empty() || (!allow_root && n == lib.root)) continue;
        if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    }
    std::vector<std::string> top;
    for (const std::string& n : out) {
        bool under = false;
        for (const std::string& m : out)
            if (m != n && IsInSubtree(lib, n, m)) under = true;
        if (!under) top.push_back(n);
    }
    return top;
}

SoundNode MakeNode(const NewNodeSpec& spec, const std::string& parent)
{
    SoundNode n;
    n.uid = NewSoundUid();
    n.parent = parent;
    n.type = spec.type;
    n.name = spec.name.empty() ? TypeName(spec.type) : spec.name;
    if (spec.type == SoundNodeType::Sound) n.sound = spec.sound;
    if (spec.type == SoundNodeType::Switch) n.group_uid = spec.group_uid;
    return n;
}

const SwitchValue* FindValue(const SoundStore& store, const std::string& group_uid, const std::string& value_uid)
{
    if (const SwitchGroup* g = FindGroup(store, group_uid))
        for (const SwitchValue& v : g->values)
            if (v.uid == value_uid) return &v;
    return nullptr;
}

std::string CaseChild(const SoundNode& sw, const std::string& value)
{
    for (const SwitchCase& c : sw.cases)
        if (c.value_uid == value) return c.child_uid;
    return {};
}

// The value a Switch case plays `child` on ("" = none).
std::string CaseChildValue(const SoundNode& sw, const std::string& child)
{
    for (const SwitchCase& c : sw.cases)
        if (c.child_uid == child) return c.value_uid;
    return {};
}

// True when the node sits in its parent Switch's Unassigned list.
bool IsUnassigned(const SoundLibrary& lib, const SoundNode& n)
{
    const SoundNode* p = n.parent.empty() || n.parent == n.uid ? nullptr : FindNode(lib, n.parent);
    return p && p->type == SoundNodeType::Switch &&
           std::find(p->unassigned.begin(), p->unassigned.end(), n.uid) != p->unassigned.end();
}

// 13-72: "Steps › LocomotionType › Run": the names from the root down to the node; a Switch
// case's content reads as its value's name, an Unassigned child gets " (Unassigned)".
std::string PathText(const SoundStore& store, const SoundLibrary& lib, const std::string& uid)
{
    std::vector<std::string> parts;
    std::set<std::string>    seen;
    for (const SoundNode* n = FindNode(lib, uid); n && seen.insert(n->uid).second;
         n = n->parent.empty() || n->parent == n->uid ? nullptr : FindNode(lib, n->parent)) {
        std::string label = n->name.empty() ? TypeName(n->type) : n->name;
        const SoundNode* p = n->parent.empty() ? nullptr : FindNode(lib, n->parent);
        if (p && p->type == SoundNodeType::Switch) {
            const std::string v = CaseChildValue(*p, n->uid);
            if (!v.empty()) {
                const SwitchValue* sv = FindValue(store, p->group_uid, v);
                label = sv ? sv->name : std::string("(deleted value)");
            } else {
                label += " (Unassigned)";
            }
        }
        parts.push_back(label);
    }
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) out += (out.empty() ? "" : " \xE2\x80\xBA ") + *it;
    return out;
}

// 13-72: an Unassigned child keeps where it was before the gesture that put it there; a node no
// longer Unassigned (or new: a copy) forgets it.
void NoteOrphans(const SoundStore& before, SoundStore& after)
{
    for (SoundLibrary& lib : after.libraries) {
        const SoundLibrary* old = FindLibrary(before, lib.uid);
        for (SoundNode& n : lib.nodes) {
            const bool now = IsUnassigned(lib, n);
            const SoundNode* o = old ? FindNode(*old, n.uid) : nullptr;
            if (now && o && IsUnassigned(*old, *o)) continue;  // already an orphan: keeps its story
            n.was_path.clear();
            n.was_switch.clear();
            n.was_value.clear();
            if (!now || !o) continue;
            n.was_path = PathText(before, *old, o->uid);
            const SoundNode* op = o->parent.empty() ? nullptr : FindNode(*old, o->parent);
            if (op && op->type == SoundNodeType::Switch) {
                n.was_switch = op->uid;
                n.was_value = CaseChildValue(*op, o->uid);
                if (n.was_value.empty()) n.was_switch.clear();
            }
        }
    }
}

// Runs an edit; restores the store and returns false when it failed or changed nothing.
bool Transact(SoundStore& store, const std::function<bool(SoundStore&)>& edit)
{
    const SoundStore before = store;
    if (!edit(store) || SerializeSoundStore(store) == SerializeSoundStore(before)) {
        store = before;
        return false;
    }
    NoteOrphans(before, store);
    return true;
}

struct InsertPoint {
    enum Mode { Into, After, ValueSlot } mode = Into;
    bool        ok = false;
    std::string cid;     // Into: the container; After: the sound's parent; ValueSlot: the Switch
    std::string after;   // After: the sound
    std::string value;   // ValueSlot
    std::string wrap;    // ValueSlot: its content
    std::string why;
};

InsertPoint ResolveInsert(const SoundLibrary& lib, const LibraryRow* row, SoundNodeType type)
{
    InsertPoint ip;
    LibraryRow r;
    if (row) r = *row;
    if (r.key.empty()) r = {"", lib.root};
    if (r.key == kHintKey) r = RowOfNode(lib, r.parent);
    if (r.key == kUnassignedKey) {
        ip.why = "Select a value or a container";
        return ip;
    }
    // A value playing a container: adds go inside it, as on the container's own row (mockup
    // insertPointFor); a value playing a Sound or nothing: its slot.
    const SoundNode* vc = RowIsValue(lib, r) ? FindNode(lib, RowNode(lib, r)) : nullptr;
    if (RowIsValue(lib, r) && !(vc && IsContainer(*vc))) {
        std::string content = RowNode(lib, r);
        if (!content.empty() && type == SoundNodeType::Sound) {
            ip.why = "This value already plays a sound. + Random / + Blend wraps it";
            return ip;
        }
        ip.ok = true;
        ip.mode = InsertPoint::ValueSlot;
        ip.cid = r.parent;
        ip.value = r.key;
        ip.wrap = content;
        return ip;
    }
    const SoundNode* n = FindNode(lib, RowNode(lib, r));
    if (!n) {
        ip.why = "The library has no root";
        return ip;
    }
    if (IsContainer(*n)) {
        if (n->type == SoundNodeType::Switch) {
            ip.why = n->uid == lib.root ? "Select a row first: the library is a Switch, its children are its values"
                                        : "A Switch holds values: select one of its value rows";
            return ip;
        }
        ip.ok = true;
        ip.mode = InsertPoint::Into;
        ip.cid = n->uid;
        return ip;
    }
    const SoundNode* p = n->parent.empty() ? nullptr : FindNode(lib, n->parent);
    if (p && p->type != SoundNodeType::Switch) {
        ip.ok = true;
        ip.mode = InsertPoint::After;
        ip.cid = p->uid;
        ip.after = n->uid;
        return ip;
    }
    ip.why = "Drag it onto a value first, or select a Random / Blend";
    return ip;
}

}  // namespace

// ---- Rows ---------------------------------------------------------------------------------------

std::string RowNode(const SoundLibrary& lib, const LibraryRow& row)
{
    if (row.parent.empty()) return (row.key == lib.root && FindNode(lib, row.key)) ? row.key : std::string();
    const SoundNode* p = FindNode(lib, row.parent);
    if (!p) return {};
    if (p->type == SoundNodeType::Switch) {
        if (std::find(p->unassigned.begin(), p->unassigned.end(), row.key) != p->unassigned.end())
            return FindNode(lib, row.key) ? row.key : std::string();
        std::string c = CaseChild(*p, row.key);
        return FindNode(lib, c) ? c : std::string();
    }
    const SoundNode* n = FindNode(lib, row.key);
    return (n && n->parent == row.parent) ? row.key : std::string();
}

bool RowIsValue(const SoundLibrary& lib, const LibraryRow& row)
{
    if (row.parent.empty() || row.key.empty() || row.key[0] == '#') return false;
    const SoundNode* p = FindNode(lib, row.parent);
    if (!p || p->type != SoundNodeType::Switch) return false;
    return std::find(p->unassigned.begin(), p->unassigned.end(), row.key) == p->unassigned.end();
}

LibraryRow RowOfNode(const SoundLibrary& lib, const std::string& uid)
{
    const SoundNode* n = FindNode(lib, uid);
    if (!n || uid == lib.root) return {"", uid};
    const SoundNode* p = n->parent.empty() ? nullptr : FindNode(lib, n->parent);
    if (p && p->type == SoundNodeType::Switch)
        for (const SwitchCase& c : p->cases)
            if (c.child_uid == uid) return {p->uid, c.value_uid};
    return {n->parent, uid};
}

bool IsInSubtree(const SoundLibrary& lib, const std::string& uid, const std::string& anc)
{
    std::string cur = uid;
    for (size_t guard = 0; guard <= lib.nodes.size() && !cur.empty(); ++guard) {
        if (cur == anc) return true;
        const SoundNode* n = FindNode(lib, cur);
        if (!n || n->parent == cur) return false;
        cur = n->parent;
    }
    return false;
}

namespace {

// The name a node row shows.
std::string NodeLabel(const SoundNode& n)
{
    return n.name.empty() ? (IsContainer(n) ? TypeName(n.type) : "Empty sound") : n.name;
}

// 13-17c: A to Z, case-insensitive.
bool LessNoCase(const std::string& a, const std::string& b)
{
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
    });
}

bool ContainsNoCase(const std::string& text, const std::string& part)
{
    return std::search(text.begin(), text.end(), part.begin(), part.end(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           }) != text.end();
}

void EmitChildren(std::vector<TreeRow>& rows, const SoundStore& store, const SoundLibrary& lib,
                  const std::set<std::string>& open, const std::string& cid, int depth, std::set<std::string>& seen);

void EmitNode(std::vector<TreeRow>& rows, const SoundStore& store, const SoundLibrary& lib,
              const std::set<std::string>& open, TreeRow r, std::set<std::string>& seen)
{
    const SoundNode* n = r.node.empty() ? nullptr : FindNode(lib, r.node);
    if (n) {
        r.container = IsContainer(*n);
        if (r.label.empty()) r.label = NodeLabel(*n);
        // "*": every container open (a search, 13-17c).
        r.open = r.container &&
                 (open.count("*") != 0 || (n->uid == lib.root ? !open.count("!root") : open.count(n->uid) != 0));
    }
    rows.push_back(r);
    if (n && r.open && !seen.count(n->uid)) {
        seen.insert(n->uid);
        EmitChildren(rows, store, lib, open, n->uid, r.depth + 1, seen);
    }
}

void EmitChildren(std::vector<TreeRow>& rows, const SoundStore& store, const SoundLibrary& lib,
                  const std::set<std::string>& open, const std::string& cid, int depth, std::set<std::string>& seen)
{
    const SoundNode* c = FindNode(lib, cid);
    if (!c) return;
    if (c->type == SoundNodeType::Switch) {
        const SwitchGroup* g = FindGroup(store, c->group_uid);
        const std::string dv = DefaultValue(store, *c);
        std::vector<std::string> shown;
        std::vector<const SwitchValue*> values;  // A to Z by name (13-17c)
        if (g)
            for (const SwitchValue& v : g->values) values.push_back(&v);
        std::stable_sort(values.begin(), values.end(), [](const SwitchValue* a, const SwitchValue* b) {
            return LessNoCase(a->name.empty() ? "(unnamed)" : a->name, b->name.empty() ? "(unnamed)" : b->name);
        });
        for (const SwitchValue* vp : values) {
            const SwitchValue& v = *vp;
            TreeRow r;
            r.row = {cid, v.uid};
            r.kind = TreeRowKind::Value;
            r.depth = depth;
            r.node = CaseChild(*c, v.uid);
            if (!FindNode(lib, r.node)) r.node.clear();
            r.label = v.name.empty() ? "(unnamed)" : v.name;
            r.is_default = v.uid == dv;
            shown.push_back(v.uid);
            EmitNode(rows, store, lib, open, r, seen);
        }
        for (const SwitchCase& sc : c->cases) {  // a case on a value the group no longer has
            if (std::find(shown.begin(), shown.end(), sc.value_uid) != shown.end()) continue;
            TreeRow r;
            r.row = {cid, sc.value_uid};
            r.kind = TreeRowKind::Value;
            r.depth = depth;
            r.node = FindNode(lib, sc.child_uid) ? sc.child_uid : std::string();
            r.label = "(deleted value)";
            EmitNode(rows, store, lib, open, r, seen);
        }
        if (!c->unassigned.empty()) {
            TreeRow h;
            h.row = {cid, kUnassignedKey};
            h.kind = TreeRowKind::UnassignedHeader;
            h.depth = depth;
            h.count = static_cast<int>(std::count_if(c->unassigned.begin(), c->unassigned.end(),
                                                     [&](const std::string& u) { return FindNode(lib, u) != nullptr; }));
            h.label = "Unassigned (" + std::to_string(h.count) + ") · drag them onto a value";
            rows.push_back(h);
            std::vector<const SoundNode*> loose;  // A to Z (13-17c)
            for (const std::string& u : c->unassigned)
                if (const SoundNode* un = FindNode(lib, u)) loose.push_back(un);
            std::stable_sort(loose.begin(), loose.end(),
                             [](const SoundNode* a, const SoundNode* b) { return LessNoCase(NodeLabel(*a), NodeLabel(*b)); });
            for (const SoundNode* un : loose) {
                TreeRow r;
                r.row = {cid, un->uid};
                r.depth = depth;
                r.node = un->uid;
                r.unassigned = true;
                EmitNode(rows, store, lib, open, r, seen);
            }
        }
        return;  // values are added in the Switch Groups column only (Antho 2026-10-12)
    }
    std::vector<const SoundNode*> kids = ChildrenOf(lib, cid);
    std::stable_sort(kids.begin(), kids.end(),  // A to Z (13-17c)
                     [](const SoundNode* a, const SoundNode* b) { return LessNoCase(NodeLabel(*a), NodeLabel(*b)); });
    for (const SoundNode* k : kids) {
        TreeRow r;
        r.row = {cid, k->uid};
        r.depth = depth;
        r.node = k->uid;
        EmitNode(rows, store, lib, open, r, seen);
    }
    if (kids.empty() && cid != lib.root) {
        TreeRow h;
        h.row = {cid, kHintKey};
        h.kind = TreeRowKind::Hint;
        h.depth = depth;
        h.label = "Drop sounds here or use + Sound";
        rows.push_back(h);
    }
}

}  // namespace

std::vector<TreeRow> BuildTreeRows(const SoundStore& store, const SoundLibrary& lib, const std::set<std::string>& open,
                                   const std::string& search)
{
    std::vector<TreeRow> rows;
    if (!FindNode(lib, lib.root)) return rows;
    std::set<std::string> seen;
    TreeRow r;
    r.row = {"", lib.root};
    r.node = lib.root;
    EmitNode(rows, store, lib, search.empty() ? open : std::set<std::string>{"*"}, r, seen);
    if (search.empty()) return rows;

    // 13-17c: keep the matching rows and their ancestors. Backwards, `below[d]` = a row kept since
    // the last row at depth <= d - 1, i.e. under the ancestor at depth d - 1. An Unassigned header
    // sits at its rows' depth, just before them: it stays when one of its rows stays.
    std::vector<char>          keep(rows.size(), 0);
    std::vector<char>          below;
    std::set<std::string>      loose_kept;  // Switches with an Unassigned row kept
    for (size_t i = rows.size(); i-- > 0;) {
        const TreeRow& t = rows[i];
        const size_t   d = static_cast<size_t>(std::max(0, t.depth));
        if (below.size() < d + 2) below.resize(d + 2, 0);
        bool under = false;
        for (size_t k = d + 1; k < below.size(); ++k) {
            under = under || below[k] != 0;
            below[k] = 0;
        }
        bool k = false;
        if (t.kind == TreeRowKind::Node || t.kind == TreeRowKind::Value) {
            const SoundNode* n = t.node.empty() ? nullptr : FindNode(lib, t.node);
            k = under || ContainsNoCase(t.label, search) ||
                (t.kind == TreeRowKind::Value && n && ContainsNoCase(NodeLabel(*n), search));
            if (k && t.unassigned) loose_kept.insert(t.row.parent);
        } else if (t.kind == TreeRowKind::UnassignedHeader) {
            k = loose_kept.count(t.row.parent) != 0;
        }
        keep[i] = k ? 1 : 0;
        if (k) below[d] = 1;
    }
    std::vector<TreeRow> out;
    for (size_t i = 0; i < rows.size(); ++i)
        if (keep[i]) out.push_back(rows[i]);
    // A container shows open only when a row is kept under it.
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i].open) out[i].open = i + 1 < out.size() && out[i + 1].depth > out[i].depth;
    return out;
}

std::vector<LibraryRow> RangeSelectByLevel(const std::vector<TreeRow>& rows, const LibraryRow& anchor,
                                           const LibraryRow& target)
{
    std::vector<LibraryRow> out;
    int ia = -1, it = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].row == anchor) ia = static_cast<int>(i);
        if (rows[i].row == target) it = static_cast<int>(i);
    }
    if (ia < 0 || it < 0) return out;
    const int d = rows[static_cast<size_t>(ia)].depth;
    auto selectable = [&](int i) {
        const TreeRow& r = rows[static_cast<size_t>(i)];
        return r.depth == d && (r.kind == TreeRowKind::Node || r.kind == TreeRowKind::Value);
    };
    const int step = it >= ia ? 1 : -1;
    while (it != ia && !selectable(it)) it -= step;  // the target's row at the anchor's depth
    const int lo = std::min(ia, it), hi = std::max(ia, it);
    for (int i = lo; i <= hi; ++i)
        if (selectable(i)) out.push_back(rows[static_cast<size_t>(i)].row);
    return out;
}

// ---- Add ----------------------------------------------------------------------------------------

bool CanAddAt(const SoundStore& store, const SoundLibrary& lib, const LibraryRow* row, SoundNodeType type,
              std::string* why)
{
    (void)store;
    InsertPoint ip = ResolveInsert(lib, row, type);
    if (why) *why = ip.why;
    return ip.ok;
}

bool AddNodes(SoundStore& store, const std::string& lib_uid, const LibraryRow* row, const std::vector<NewNodeSpec>& specs,
              std::vector<std::string>* created, std::string* why)
{
    if (specs.empty()) return false;
    std::vector<std::string> made;
    bool ok = Transact(store, [&](SoundStore& s) {
        SoundLibrary* lib = FindLibrary(s, lib_uid);
        if (!lib) return false;
        for (const NewNodeSpec& sp : specs)
            if (sp.type == SoundNodeType::Switch && !FindGroup(s, sp.group_uid)) {
                if (why) *why = "Pick a Switch Group";
                return false;
            }
        InsertPoint ip = ResolveInsert(*lib, row, specs.front().type);
        if (why) *why = ip.why;
        if (!ip.ok) return false;
        auto add_switch_group = [&](const std::string& uid, const NewNodeSpec& sp) {
            if (sp.type == SoundNodeType::Switch) {
                SoundNode* n = FindNode(*lib, uid);
                n->type = SoundNodeType::Random;
                n->group_uid.clear();
                ConvertNodeType(*lib, uid, SoundNodeType::Switch, sp.group_uid, s);
            }
        };
        if (ip.mode == InsertPoint::Into) {
            for (const NewNodeSpec& sp : specs) {
                lib->nodes.push_back(MakeNode(sp, ip.cid));
                made.push_back(lib->nodes.back().uid);
            }
            return true;
        }
        if (ip.mode == InsertPoint::After) {
            std::string after = ip.after;
            for (const NewNodeSpec& sp : specs) {
                SoundNode n = MakeNode(sp, ip.cid);
                std::string uid = n.uid;
                lib->nodes.push_back(std::move(n));
                MoveLineAfter(*lib, uid, after);
                after = uid;
                made.push_back(uid);
            }
            return true;
        }
        // A Switch value.
        Slot slot;
        slot.parent = ip.cid;
        slot.value = ip.value;
        const SoundNode* sw = FindNode(*lib, ip.cid);
        const SwitchValue* v = sw ? FindValue(s, sw->group_uid, ip.value) : nullptr;
        if (!ip.wrap.empty()) {  // a container wraps what the value played
            const NewNodeSpec& sp = specs.front();
            NewNodeSpec as_random = sp;
            if (as_random.type == SoundNodeType::Switch) as_random.type = SoundNodeType::Random;
            SoundNode w = MakeNode(as_random, ip.cid);
            std::string wu = w.uid;
            lib->nodes.push_back(std::move(w));
            Detach(*lib, ip.wrap);
            Attach(*lib, wu, slot);
            FindNode(*lib, ip.wrap)->parent = wu;
            if (sp.type == SoundNodeType::Switch) ConvertNodeType(*lib, wu, SoundNodeType::Switch, sp.group_uid, s);
            made.push_back(wu);
            return true;
        }
        if (specs.size() == 1) {
            SoundNode n = MakeNode(specs.front(), ip.cid);
            std::string uid = n.uid;
            lib->nodes.push_back(std::move(n));
            Attach(*lib, uid, slot);
            add_switch_group(uid, specs.front());
            made.push_back(uid);
            return true;
        }
        NewNodeSpec rs;
        rs.type = SoundNodeType::Random;
        rs.name = v ? v->name : std::string();
        SoundNode holder = MakeNode(rs, ip.cid);
        std::string hu = holder.uid;
        lib->nodes.push_back(std::move(holder));
        Attach(*lib, hu, slot);
        made.push_back(hu);
        for (const NewNodeSpec& sp : specs) {
            lib->nodes.push_back(MakeNode(sp, hu));
            made.push_back(lib->nodes.back().uid);
        }
        return true;
    });
    if (ok && created) *created = made;
    return ok;
}

// ---- Remove / move ------------------------------------------------------------------------------

bool RemoveRows(SoundStore& store, const std::string& lib_uid, const std::vector<LibraryRow>& rows)
{
    return Transact(store, [&](SoundStore& s) {
        SoundLibrary* lib = FindLibrary(s, lib_uid);
        if (!lib) return false;
        for (const std::string& n : RowNodes(*lib, rows, false)) EraseSubtree(*lib, n);
        return true;
    });
}

namespace {

struct ResolvedDrop {
    bool        ok = false;
    std::string container;  // Into: the container; OntoValue: the Switch; Before / After: the parent
    std::string sibling;    // Before / After
    bool        sibling_unassigned = false;
};

ResolvedDrop ResolveDrop(const SoundLibrary& dst, const DropTarget& to)
{
    ResolvedDrop d;
    LibraryRow row = to.row;
    DropMode mode = to.mode;
    if (row.key == kHintKey) {
        row = RowOfNode(dst, row.parent);
        mode = DropMode::Into;
    }
    if (mode == DropMode::OntoValue) {
        if (!RowIsValue(dst, row)) return d;
        d.container = row.parent;
        d.ok = true;
        return d;
    }
    if (mode == DropMode::Into) {
        const SoundNode* c = FindNode(dst, RowNode(dst, row));
        if (!c || c->type == SoundNodeType::Sound || c->type == SoundNodeType::Switch) return d;
        d.container = c->uid;
        d.ok = true;
        return d;
    }
    if (RowIsValue(dst, row)) return d;
    const SoundNode* sib = FindNode(dst, RowNode(dst, row));
    if (!sib || sib->uid == dst.root) return d;
    const SoundNode* p = FindNode(dst, sib->parent);
    if (!p) return d;
    d.container = p->uid;
    d.sibling = sib->uid;
    d.sibling_unassigned = p->type == SoundNodeType::Switch;
    d.ok = true;
    return d;
}

bool DropAllowed(const SoundLibrary& src, const std::vector<std::string>& nodes, const SoundLibrary& dst,
                 const DropTarget& to, const ResolvedDrop& d, bool copy)
{
    if (!d.ok || nodes.empty()) return false;
    if (&src != &dst) return true;
    // A value's content onto its own value (13-70 review: a copy of several rows too, else it
    // would push the original out to Unassigned).
    if (to.mode == DropMode::OntoValue && (nodes.size() == 1 || copy))
        if (const SoundNode* sw = FindNode(dst, d.container)) {
            const std::string held = CaseChild(*sw, to.row.key);
            if (!held.empty() && std::find(nodes.begin(), nodes.end(), held) != nodes.end()) return false;
        }
    if (copy) return true;  // 13-70: the copy is taken first, so into / next to itself is fine
    for (const std::string& n : nodes) {
        if (IsInSubtree(dst, d.container, n)) return false;  // into itself or a descendant
        if (n == d.sibling) return false;
    }
    return true;
}

}  // namespace

bool CanDrop(const SoundStore& store, const std::string& src_lib, const std::vector<LibraryRow>& rows,
             const DropTarget& to, bool copy)
{
    const SoundLibrary* src = FindLibrary(store, src_lib);
    const SoundLibrary* dst = FindLibrary(store, to.lib);
    if (!src || !dst) return false;
    std::vector<std::string> nodes = RowNodes(*src, rows, false);
    return DropAllowed(*src, nodes, *dst, to, ResolveDrop(*dst, to), copy);
}

namespace {

// MoveRows' body; `copy` = Copy / Paste (CopySubtree each row, the originals stay).
bool PlaceRows(SoundStore& store, const std::string& src_lib, const std::vector<LibraryRow>& rows, const DropTarget& to,
               bool copy, std::vector<std::string>* moved)
{
    std::vector<std::string> out;
    bool ok = Transact(store, [&](SoundStore& s) {
        SoundLibrary* src = FindLibrary(s, src_lib);
        SoundLibrary* dst = FindLibrary(s, to.lib);
        if (!src || !dst) return false;
        std::vector<std::string> nodes = RowNodes(*src, rows, false);
        ResolvedDrop d = ResolveDrop(*dst, to);
        if (!DropAllowed(*src, nodes, *dst, to, d, copy)) return false;
        const bool same = src == dst;

        // Onto a value: what it held before (unless it is one of the moved rows).
        std::string old;
        if (to.mode == DropMode::OntoValue) {
            const SoundNode* sw = FindNode(*dst, d.container);
            old = sw ? CaseChild(*sw, to.row.key) : std::string();
            if (!copy && same && std::find(nodes.begin(), nodes.end(), old) != nodes.end()) old.clear();
        }
        Slot from;
        if (!copy && same && nodes.size() == 1) from = SlotOf(*src, nodes.front());

        // Take the moved nodes out (or copy them over).
        std::vector<std::string> placed;
        for (const std::string& n : nodes) {
            if (copy) {  // 13-70: the original stays; the copy's top is placed below like a moved node
                placed.push_back(CopySubtree(*src, n, *dst));
            } else if (same) {
                Detach(*src, n);
                placed.push_back(n);
            } else {
                placed.push_back(CopySubtree(*src, n, *dst));
                EraseSubtree(*src, n);
            }
        }

        if (to.mode == DropMode::OntoValue) {
            Slot vs;
            vs.parent = d.container;
            vs.value = to.row.key;
            if (!old.empty()) Detach(*dst, old);
            if (placed.size() == 1) {
                Attach(*dst, placed.front(), vs);
            } else {
                const SoundNode* sw = FindNode(*dst, d.container);
                const SwitchValue* v = sw ? FindValue(s, sw->group_uid, to.row.key) : nullptr;
                NewNodeSpec rs;
                rs.type = SoundNodeType::Random;
                rs.name = v ? v->name : std::string();
                SoundNode holder = MakeNode(rs, d.container);
                std::string hu = holder.uid;
                dst->nodes.push_back(std::move(holder));
                Attach(*dst, hu, vs);
                for (const std::string& p : placed) {
                    FindNode(*dst, p)->parent = hu;
                    MoveLineBefore(*dst, p, "");
                }
            }
            if (!old.empty()) {
                Slot back;
                back.parent = d.container;
                back.unassigned = true;
                const bool can_swap = same && placed.size() == 1 && !from.parent.empty() &&
                                      !IsInSubtree(*dst, from.parent, old) && FindNode(*dst, from.parent);
                if (can_swap) back = from;
                Attach(*dst, old, back);
            }
        } else if (to.mode == DropMode::Into || to.row.key == kHintKey) {  // a hint row = into its container
            for (const std::string& p : placed) {
                FindNode(*dst, p)->parent = d.container;
                MoveLineBefore(*dst, p, "");
            }
        } else {
            const bool after = to.mode == DropMode::After;
            if (d.sibling_unassigned) {
                for (size_t i = 0; i < placed.size(); ++i) {
                    const SoundNode* sw = FindNode(*dst, d.container);
                    auto it = std::find(sw->unassigned.begin(), sw->unassigned.end(), d.sibling);
                    const long base = it - sw->unassigned.begin();
                    Slot us;
                    us.parent = d.container;
                    us.unassigned = true;
                    us.unassigned_idx = static_cast<int>(after ? base + 1 + static_cast<long>(i) : base);
                    Attach(*dst, placed[i], us);
                }
            } else {
                std::string ref = d.sibling;
                for (const std::string& p : placed) {
                    FindNode(*dst, p)->parent = d.container;
                    if (after) {
                        MoveLineAfter(*dst, p, ref);
                        ref = p;
                    } else {
                        MoveLineBefore(*dst, p, ref);
                    }
                }
            }
        }
        out = placed;
        return true;
    });
    if (ok && moved) *moved = out;
    return ok;
}

}  // namespace

bool MoveRows(SoundStore& store, const std::string& src_lib, const std::vector<LibraryRow>& rows, const DropTarget& to,
              std::vector<std::string>* moved)
{
    return PlaceRows(store, src_lib, rows, to, false, moved);
}

bool CopyRows(SoundStore& store, const std::string& src_lib, const std::vector<LibraryRow>& rows, const DropTarget& to,
              std::vector<std::string>* created)
{
    return PlaceRows(store, src_lib, rows, to, true, created);
}

// ---- Wrap / group / duplicate -------------------------------------------------------------------

namespace {

// A new container `w` takes node `n`'s place; `n` goes inside it.
std::string WrapOne(SoundStore& s, SoundLibrary& lib, const std::string& n, SoundNodeType type,
                    const std::string& group_uid)
{
    Slot slot = SlotOf(lib, n);
    if (n != lib.root && slot.parent.empty()) return {};
    NewNodeSpec sp;
    sp.type = SoundNodeType::Random;
    sp.name = TypeName(type);
    SoundNode w = MakeNode(sp, slot.parent);
    std::string wu = w.uid;
    lib.nodes.push_back(std::move(w));
    if (n == lib.root) {
        MoveLineBefore(lib, wu, n);
        lib.root = wu;
    } else {
        // Its place: just before n in line order, in n's Switch slot.
        if (!slot.value.empty() || slot.unassigned) {
            Detach(lib, n);
            Attach(lib, wu, slot);
        }
        MoveLineBefore(lib, wu, n);
    }
    SoundNode* wn = FindNode(lib, n);
    wn->parent = wu;
    {  // 13-31: the wrapper takes the layer's place in a Blend, so its roles too
        SoundNode* wr = FindNode(lib, wu);
        wr->layer_roles = std::move(wn->layer_roles);
        wr->layer_keep_offset = wn->layer_keep_offset;
        wn->layer_roles.clear();
        wn->layer_keep_offset = false;
    }
    if (type == SoundNodeType::Blend) FindNode(lib, wu)->type = SoundNodeType::Blend;
    if (type == SoundNodeType::Switch) {
        ConvertNodeType(lib, wu, SoundNodeType::Switch, group_uid, s);
        // 13-72 (Antho 2026-10-19): what is wrapped in a Switch plays on the group's default value
        // (unless its name already matched a value), not Unassigned, so the tree keeps playing.
        SoundNode*         sw = FindNode(lib, wu);
        const SwitchGroup* gr = FindGroup(s, group_uid);
        const std::string  dv = gr ? GroupDefaultValue(*gr) : std::string();
        if (sw && !dv.empty() && CaseChild(*sw, dv).empty() &&
            std::find(sw->unassigned.begin(), sw->unassigned.end(), n) != sw->unassigned.end()) {
            sw->unassigned.erase(std::remove(sw->unassigned.begin(), sw->unassigned.end(), n), sw->unassigned.end());
            sw->cases.push_back({dv, n, {}});
            sw->default_value.clear();  // it plays the group's default: follows it
        }
    }
    return wu;
}

}  // namespace

bool WrapRows(SoundStore& store, const std::string& lib_uid, const std::vector<LibraryRow>& rows, SoundNodeType type,
              const std::string& group_uid, std::vector<std::string>* created)
{
    if (type == SoundNodeType::Sound) return false;
    std::vector<std::string> made;
    bool ok = Transact(store, [&](SoundStore& s) {
        SoundLibrary* lib = FindLibrary(s, lib_uid);
        if (!lib) return false;
        if (type == SoundNodeType::Switch && !FindGroup(s, group_uid)) return false;
        // 13-72 (Wrap each in): the rows' nodes resolved before any wrap (a wrapped value row then
        // shows its wrapper), each once.
        std::vector<std::string> nodes;
        for (const LibraryRow& r : rows) {
            std::string n = RowNode(*lib, r);
            if (!n.empty() && std::find(nodes.begin(), nodes.end(), n) == nodes.end()) nodes.push_back(n);
        }
        for (const std::string& n : nodes) {
            std::string w = WrapOne(s, *lib, n, type, group_uid);
            if (!w.empty()) made.push_back(w);
        }
        return !made.empty();
    });
    if (ok && created) *created = made;
    return ok;
}

bool CanUnwrap(const SoundLibrary& lib, const std::string& uid)
{
    const SoundNode* c = FindNode(lib, uid);
    return c && c->type != SoundNodeType::Sound && ChildrenOf(lib, uid).size() == 1;
}

bool UnwrapRows(SoundStore& store, const std::string& lib_uid, const std::vector<LibraryRow>& rows)
{
    return Transact(store, [&](SoundStore& s) {
        SoundLibrary* lib = FindLibrary(s, lib_uid);
        if (!lib) return false;
        bool done = false;
        for (const std::string& cu : RowNodes(*lib, rows, /*allow_root=*/true)) {
            if (!CanUnwrap(*lib, cu)) continue;
            const SoundNode   c = *FindNode(*lib, cu);
            const std::string ku = ChildrenOf(*lib, cu).front()->uid;
            const Slot        slot = SlotOf(*lib, cu);
            Detach(*lib, ku);  // off the container's cases (a Switch)
            SoundNode* k = FindNode(*lib, ku);
            k->volume_db += c.volume_db;
            k->offset_ms = c.offset_ms + (c.type == SoundNodeType::Blend ? k->offset_ms : 0.0);
            if (k->lands_in.empty()) k->lands_in = c.lands_in;
            if (const SoundNode* cp = c.parent.empty() ? nullptr : FindNode(*lib, c.parent))
                if (cp->type == SoundNodeType::Blend) {  // 13-31: the node takes the layer's place, its roles too
                    k->layer_roles = c.layer_roles;
                    k->layer_keep_offset = c.layer_keep_offset;
                }
            if (cu == lib->root) {
                k->parent.clear();
                lib->root = ku;
            } else {
                Detach(*lib, cu);
                k->parent = slot.parent;
                SoundNode* p = FindNode(*lib, slot.parent);
                if (p && p->type == SoundNodeType::Switch) Attach(*lib, ku, slot);
            }
            MoveLineBefore(*lib, ku, cu);
            lib->nodes.erase(lib->nodes.begin() + IndexOf(*lib, cu));
            done = true;
        }
        return done;
    });
}

bool GroupRows(SoundStore& store, const std::string& lib_uid, const std::vector<LibraryRow>& rows, SoundNodeType type,
               const std::string& group_uid, std::string* created)
{
    if (type == SoundNodeType::Sound) return false;
    std::string made;
    bool ok = Transact(store, [&](SoundStore& s) {
        SoundLibrary* lib = FindLibrary(s, lib_uid);
        if (!lib) return false;
        if (type == SoundNodeType::Switch && !FindGroup(s, group_uid)) return false;
        std::vector<std::string> nodes = RowNodes(*lib, rows, true);
        if (nodes.empty()) return false;
        if (std::find(nodes.begin(), nodes.end(), lib->root) != nodes.end() && nodes.size() > 1) return false;
        // As a Random first; turned into a Switch once it holds them all.
        made = WrapOne(s, *lib, nodes.front(), SoundNodeType::Random, std::string());
        if (made.empty()) return false;
        for (size_t i = 1; i < nodes.size(); ++i) {
            Detach(*lib, nodes[i]);
            FindNode(*lib, nodes[i])->parent = made;
            MoveLineBefore(*lib, nodes[i], "");
        }
        if (type == SoundNodeType::Blend) FindNode(*lib, made)->type = SoundNodeType::Blend;
        if (type == SoundNodeType::Switch) ConvertNodeType(*lib, made, SoundNodeType::Switch, group_uid, s);
        FindNode(*lib, made)->name = TypeName(type);
        return true;
    });
    if (ok && created) *created = made;
    return ok;
}

bool DuplicateRows(SoundStore& store, const std::string& lib_uid, const std::vector<LibraryRow>& rows,
                   std::vector<std::string>* created)
{
    std::vector<std::string> made;
    bool ok = Transact(store, [&](SoundStore& s) {
        SoundLibrary* lib = FindLibrary(s, lib_uid);
        if (!lib) return false;
        for (const std::string& n : RowNodes(*lib, rows, false)) {
            Slot slot = SlotOf(*lib, n);
            std::string c = CopySubtree(*lib, n, *lib);
            if (!slot.value.empty()) {
                Slot us;
                us.parent = slot.parent;
                us.unassigned = true;
                Attach(*lib, c, us);
            } else if (slot.unassigned) {
                slot.unassigned_idx += 1;
                Attach(*lib, c, slot);
            }
            MoveLineAfter(*lib, c, n);
            made.push_back(c);
        }
        return !made.empty();
    });
    if (ok && created) *created = made;
    return ok;
}

bool RenameRow(SoundStore& store, const std::string& lib_uid, const LibraryRow& row, const std::string& name)
{
    if (name.empty()) return false;
    // A value row shows the value's name: renaming it renames the value (in every library, as
    // the mockup's commitRename does).
    if (const SoundLibrary* vl = FindLibrary(store, lib_uid))
        if (RowIsValue(*vl, row))
            if (const SoundNode* sw = FindNode(*vl, row.parent))
                if (FindValue(store, sw->group_uid, row.key)) {
                    const std::string gid = sw->group_uid;
                    return RenameValue(store, gid, row.key, name);
                }
    return Transact(store, [&](SoundStore& s) {
        SoundLibrary* lib = FindLibrary(s, lib_uid);
        SoundNode* n = lib ? FindNode(*lib, RowNode(*lib, row)) : nullptr;
        if (!n) return false;
        n->name = name;
        return true;
    });
}

std::string RowTypeNode(const SoundLibrary& lib, const LibraryRow& row)
{
    const std::string n = RowNode(lib, row);
    const SoundNode* nn = n.empty() ? nullptr : FindNode(lib, n);
    if (nn && IsContainer(*nn)) return n;
    if (RowIsValue(lib, row)) {
        const SoundNode* sw = FindNode(lib, row.parent);
        if (sw && sw->type == SoundNodeType::Switch) return sw->uid;
    }
    return {};
}

bool SetRowsType(SoundStore& store, const std::string& lib_uid, const std::vector<LibraryRow>& rows, SoundNodeType type,
                 const std::string& group_uid)
{
    return Transact(store, [&](SoundStore& s) {
        SoundLibrary* lib = FindLibrary(s, lib_uid);
        if (!lib) return false;
        // Resolved before any conversion: a converted Switch's value rows change meaning.
        std::vector<std::string> targets;
        for (const LibraryRow& r : rows) {
            std::string n = RowTypeNode(*lib, r);
            if (!n.empty() && std::find(targets.begin(), targets.end(), n) == targets.end()) targets.push_back(n);
        }
        bool any = false;
        for (const std::string& n : targets)
            if (ConvertNodeType(*lib, n, type, group_uid, s)) any = true;
        return any;
    });
}

bool MakeDefault(SoundStore& store, const std::string& lib_uid, const std::vector<LibraryRow>& rows)
{
    return Transact(store, [&](SoundStore& s) {
        SoundLibrary* lib = FindLibrary(s, lib_uid);
        if (!lib) return false;
        for (const LibraryRow& r : rows) {
            if (!RowIsValue(*lib, r)) continue;
            if (SoundNode* sw = FindNode(*lib, r.parent))
                if (FindValue(s, sw->group_uid, r.key)) sw->default_value = r.key;  // never a "(deleted value)"
        }
        return true;
    });
}

bool FollowGroupDefault(SoundStore& store, const std::string& lib_uid, const std::string& switch_uid)
{
    SoundLibrary* lib = FindLibrary(store, lib_uid);
    SoundNode*    sw = lib ? FindNode(*lib, switch_uid) : nullptr;
    if (!sw || sw->type != SoundNodeType::Switch || sw->default_value.empty()) return false;
    sw->default_value.clear();
    return true;
}

bool ClearValue(SoundStore& store, const std::string& lib_uid, const LibraryRow& row)
{
    return Transact(store, [&](SoundStore& s) {
        SoundLibrary* lib = FindLibrary(s, lib_uid);
        if (!lib || !RowIsValue(*lib, row)) return false;
        std::string c = RowNode(*lib, row);
        if (c.empty()) return false;
        Detach(*lib, c);
        Slot us;
        us.parent = row.parent;
        us.unassigned = true;
        Attach(*lib, c, us);
        return true;
    });
}

// ---- 13-72: wrap value contents, put back, paste onto --------------------------------------------

std::vector<LibraryRow> ValueContentRows(const SoundLibrary& lib, const std::vector<LibraryRow>& rows)
{
    std::vector<LibraryRow> out;
    auto add = [&](const LibraryRow& r) {
        if (!RowNode(lib, r).empty() && std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
    };
    for (const LibraryRow& r : rows) {
        if (RowIsValue(lib, r)) {
            add(r);
            continue;
        }
        const SoundNode* sw = FindNode(lib, RowNode(lib, r));
        if (!sw || sw->type != SoundNodeType::Switch) continue;
        for (const SwitchCase& c : sw->cases)
            if (FindNode(lib, c.child_uid)) add({sw->uid, c.value_uid});
    }
    return out;
}

bool WrapValueContents(SoundStore& store, const std::string& lib_uid, const std::vector<LibraryRow>& rows,
                       SoundNodeType type, const std::string& group_uid, std::vector<std::string>* created)
{
    const SoundLibrary* lib = FindLibrary(store, lib_uid);
    if (!lib) return false;
    const std::vector<LibraryRow> targets = ValueContentRows(*lib, rows);
    if (targets.empty()) return false;
    return WrapRows(store, lib_uid, targets, type, group_uid, created);
}

bool CanPutBack(const SoundStore& store, const SoundLibrary& lib, const std::string& uid, std::string* why)
{
    auto no = [&](const char* w) {
        if (why) *why = w;
        return false;
    };
    const SoundNode* n = FindNode(lib, uid);
    if (!n || !IsUnassigned(lib, *n)) return no("Only an Unassigned row can be put back");
    if (n->was_switch.empty() || n->was_value.empty()) return no("It was not on a Switch value before");
    const SoundNode* sw = FindNode(lib, n->was_switch);
    if (!sw || sw->type != SoundNodeType::Switch) return no("The Switch it was on no longer exists");
    const SwitchGroup* gr = FindGroup(store, sw->group_uid);
    if (!gr || !GroupHasValue(*gr, n->was_value)) return no("Its value no longer exists on that Switch");
    if (IsInSubtree(lib, sw->uid, uid)) return no("The Switch it was on is now inside it");
    if (CaseChild(*sw, n->was_value) == uid) return no("It already plays that value");
    if (why) why->clear();
    return true;
}

bool PutBackRows(SoundStore& store, const std::string& lib_uid, const std::vector<LibraryRow>& rows,
                 std::vector<std::string>* placed)
{
    std::vector<std::string> out;
    bool ok = Transact(store, [&](SoundStore& s) {
        SoundLibrary* lib = FindLibrary(s, lib_uid);
        if (!lib) return false;
        std::vector<std::string> nodes;
        for (const LibraryRow& r : rows) {
            const std::string n = RowNode(*lib, r);
            if (!n.empty() && std::find(nodes.begin(), nodes.end(), n) == nodes.end()) nodes.push_back(n);
        }
        for (const std::string& n : nodes) {
            if (!CanPutBack(s, *lib, n, nullptr)) continue;  // checked again: an earlier one may have moved
            const SoundNode*  nn = FindNode(*lib, n);
            const std::string sw = nn->was_switch, v = nn->was_value;
            // What the value plays now goes to that Switch's Unassigned (Paste's rule on a taken value).
            const std::string old = CaseChild(*FindNode(*lib, sw), v);
            Detach(*lib, n);
            if (!old.empty()) {
                Detach(*lib, old);
                Slot us;
                us.parent = sw;
                us.unassigned = true;
                Attach(*lib, old, us);
            }
            Slot vs;
            vs.parent = sw;
            vs.value = v;
            Attach(*lib, n, vs);
            out.push_back(n);
        }
        return !out.empty();
    });
    if (ok && placed) *placed = out;
    return ok;
}

std::vector<std::pair<std::string, std::string>> OrphanUsedBy(const SoundStore& store, const std::string& lib_uid,
                                                              const SoundNode& n)
{
    std::vector<std::pair<std::string, std::string>> out;
    if (n.was_value.empty()) return out;
    auto scan = [&](const std::vector<SoundAssign>& list, const std::string& who) {
        for (const SoundAssign& a : list) {
            // 13-72 review: a target that plays no library, or another library, does not use it.
            if (a.state == LibState::None || (a.state == LibState::Library && a.library != lib_uid)) continue;
            for (const ValueSet& v : a.values)
                if (v.value_uid == n.was_value) out.emplace_back(who, a.key);
        }
    };
    scan(store.project, std::string());
    for (const SoundCharacter& c : store.characters) scan(c.assigns, c.name.empty() ? std::string("Character") : c.name);
    return out;
}

bool CanPasteOnto(const SoundStore& store, const std::string& src_lib, const std::string& src_uid,
                  const std::string& dst_lib, const std::string& dst_uid, PasteOntoWhat what, std::string* why)
{
    auto no = [&](const char* w) {
        if (why) *why = w;
        return false;
    };
    const SoundLibrary* sl = FindLibrary(store, src_lib);
    const SoundLibrary* dl = FindLibrary(store, dst_lib);
    const SoundNode*    sn = sl ? FindNode(*sl, src_uid) : nullptr;
    const SoundNode*    tn = dl ? FindNode(*dl, dst_uid) : nullptr;
    if (!sn) return no("The copied row no longer exists");
    if (!tn) return no("Right-click the container to replace");
    if (sl == dl && sn->uid == tn->uid) return no("That is the copied row itself");
    const bool sc = IsContainer(*sn), tc = IsContainer(*tn);
    if (what == PasteOntoWhat::Contents) {
        if (!sc || !tc) return no("Sounds have no contents: use Paste");
        if (sl == dl && (IsInSubtree(*dl, tn->uid, sn->uid) || IsInSubtree(*dl, sn->uid, tn->uid)))
            return no("One is inside the other");
    } else if (sc != tc) {
        return no("A Sound and a container do not share settings");
    }
    if (why) why->clear();
    return true;
}

bool PasteOnto(SoundStore& store, const std::string& src_lib, const std::string& src_uid, const std::string& dst_lib,
               const std::string& dst_uid, PasteOntoWhat what)
{
    if (!CanPasteOnto(store, src_lib, src_uid, dst_lib, dst_uid, what, nullptr)) return false;
    return Transact(store, [&](SoundStore& s) {
        SoundLibrary* src = FindLibrary(s, src_lib);
        SoundLibrary* dst = FindLibrary(s, dst_lib);
        const SoundNode from = *FindNode(*src, src_uid);  // a copy: dst may grow below
        if (what == PasteOntoWhat::Settings) {
            SoundNode* t = FindNode(*dst, dst_uid);
            t->volume_db = from.volume_db;
            t->avoid_repeat = from.avoid_repeat;
            if (from.type == SoundNodeType::Switch && t->type == SoundNodeType::Switch && from.group_uid == t->group_uid)
                t->default_value = from.default_value;
            return true;
        }
        // Contents: the target keeps its place, name, settings and assignments; its children are
        // replaced by a copy of the source's (CopySubtree), and it takes the source's type (and Switch
        // Group, cases, default, Unassigned for a Switch).
        std::vector<std::string> old_kids;
        for (const SoundNode* k : ChildrenOf(*dst, dst_uid)) old_kids.push_back(k->uid);
        for (const std::string& k : old_kids) EraseSubtree(*dst, k);
        std::vector<std::string> src_kids;
        for (const SoundNode* k : ChildrenOf(*src, src_uid)) src_kids.push_back(k->uid);
        std::vector<std::pair<std::string, std::string>> map;  // source child -> its copy
        for (const std::string& k : src_kids) {
            const std::string c = CopySubtree(*src, k, *dst);
            FindNode(*dst, c)->parent = dst_uid;
            map.emplace_back(k, c);
        }
        auto copy_of = [&](const std::string& u) {
            for (const auto& m : map)
                if (m.first == u) return m.second;
            return std::string();
        };
        SoundNode* t = FindNode(*dst, dst_uid);
        t->type = from.type;
        t->cases.clear();
        t->unassigned.clear();
        t->group_uid.clear();
        t->default_value.clear();
        t->preview_value.clear();
        if (from.type == SoundNodeType::Switch) {
            t->group_uid = from.group_uid;
            t->default_value = from.default_value;
            t->preview_value = from.preview_value;
            for (const SwitchCase& c : from.cases) {
                const std::string k = copy_of(c.child_uid);
                if (!k.empty()) t->cases.push_back({c.value_uid, k, c.kept});
            }
            for (const std::string& u : from.unassigned) {
                const std::string k = copy_of(u);
                if (!k.empty()) t->unassigned.push_back(k);
            }
        }
        return true;
    });
}

// ---- Switch Groups ------------------------------------------------------------------------------

std::string UniqueName(const std::string& base, const std::vector<std::string>& taken)
{
    auto used = [&](const std::string& n) {
        for (const std::string& t : taken)
            if (SwitchNameKey(t) == SwitchNameKey(n)) return true;
        return false;
    };
    if (!used(base)) return base;
    for (int i = 2;; ++i) {
        std::string n = base + " " + std::to_string(i);
        if (!used(n)) return n;
    }
}

bool AddGroup(SoundStore& store, const std::string& name, std::string* uid)
{
    std::vector<std::string> names;
    for (const SwitchGroup& g : store.groups) names.push_back(g.name);
    SwitchGroup g;
    g.uid = NewSoundUid();
    g.name = UniqueName(name.empty() ? "Group" : name, names);
    store.groups.push_back(g);
    if (uid) *uid = g.uid;
    return true;
}

bool RenameGroup(SoundStore& store, const std::string& group_uid, const std::string& name)
{
    SwitchGroup* g = FindGroup(store, group_uid);
    if (!g || name.empty() || g->name == name) return false;
    std::vector<std::string> names;
    for (const SwitchGroup& o : store.groups)
        if (o.uid != group_uid) names.push_back(o.name);
    const std::string unique = UniqueName(name, names);  // never two groups with one name (SwitchNameKey)
    if (g->name == unique) return false;
    g->name = unique;
    return true;
}

bool DeleteGroup(SoundStore& store, const std::string& group_uid)
{
    if (!FindGroup(store, group_uid)) return false;
    for (SoundLibrary& lib : store.libraries)
        for (size_t i = 0; i < lib.nodes.size(); ++i)
            if (lib.nodes[i].type == SoundNodeType::Switch && lib.nodes[i].group_uid == group_uid) {
                std::string uid = lib.nodes[i].uid;
                ConvertNodeType(lib, uid, SoundNodeType::Random, std::string(), store);
            }
    store.groups.erase(std::remove_if(store.groups.begin(), store.groups.end(),
                                      [&](const SwitchGroup& g) { return g.uid == group_uid; }),
                       store.groups.end());
    return true;
}

bool AddValue(SoundStore& store, const std::string& group_uid, const std::string& name, std::string* uid)
{
    SwitchGroup* g = FindGroup(store, group_uid);
    if (!g) return false;
    std::vector<std::string> names;
    for (const SwitchValue& v : g->values) names.push_back(v.name);
    SwitchValue v;
    v.uid = NewSoundUid();
    v.name = UniqueName(name.empty() ? "Value" : name, names);
    g->values.push_back(v);
    if (uid) *uid = v.uid;
    return true;
}

bool RenameValue(SoundStore& store, const std::string& group_uid, const std::string& value_uid, const std::string& name)
{
    SwitchGroup* g = FindGroup(store, group_uid);
    if (!g || name.empty()) return false;
    std::vector<std::string> names;
    for (const SwitchValue& o : g->values)
        if (o.uid != value_uid) names.push_back(o.name);
    const std::string unique = UniqueName(name, names);  // name matching (ConvertNodeType) stays unambiguous
    for (SwitchValue& v : g->values)
        if (v.uid == value_uid && v.name != unique) {
            v.name = unique;
            return true;
        }
    return false;
}

bool DeleteValue(SoundStore& store, const std::string& group_uid, const std::string& value_uid)
{
    SwitchGroup* g = FindGroup(store, group_uid);
    if (!g) return false;
    auto vit = std::find_if(g->values.begin(), g->values.end(), [&](const SwitchValue& v) { return v.uid == value_uid; });
    if (vit == g->values.end()) return false;
    g->values.erase(vit);
    if (g->default_value == value_uid) g->default_value.clear();  // 13-18 D5: back to the first value
    const std::string gdef = GroupDefaultValue(*g);
    for (SoundLibrary& lib : store.libraries)
        for (SoundNode& sw : lib.nodes) {
            if (sw.type != SoundNodeType::Switch || sw.group_uid != group_uid) continue;
            for (auto it = sw.cases.begin(); it != sw.cases.end();) {
                if (it->value_uid == value_uid) {
                    if (!it->child_uid.empty()) sw.unassigned.push_back(it->child_uid);
                    it = sw.cases.erase(it);
                } else {
                    ++it;
                }
            }
            if (sw.default_value == value_uid) {
                sw.default_value.clear();
                // 13-18: the group's default has a child here: it follows the group.
                if (gdef.empty() || CaseChild(sw, gdef).empty())
                    for (const SwitchValue& v : g->values)
                        if (!CaseChild(sw, v.uid).empty()) {
                            sw.default_value = v.uid;
                            break;
                        }
            }
            if (sw.preview_value == value_uid) sw.preview_value.clear();
        }
    return true;
}

int CountDefaultOverrides(const SoundStore& store, const std::string& group_uid, const std::string& value_uid)
{
    const SwitchGroup* g = FindGroup(store, group_uid);
    if (!g) return 0;
    int n = 0;
    for (const SoundLibrary& lib : store.libraries)
        for (const SoundNode& sw : lib.nodes) {
            if (sw.type != SoundNodeType::Switch || sw.group_uid != group_uid) continue;
            if (sw.default_value.empty() || sw.default_value == value_uid) continue;
            for (const SwitchValue& v : g->values)
                if (v.uid == sw.default_value) {
                    ++n;
                    break;
                }
        }
    return n;
}

bool SetGroupDefault(SoundStore& store, const std::string& group_uid, const std::string& value_uid, bool follow)
{
    SwitchGroup* g = FindGroup(store, group_uid);
    if (!g || !std::any_of(g->values.begin(), g->values.end(), [&](const SwitchValue& v) { return v.uid == value_uid; }))
        return false;
    bool changed = g->default_value != value_uid;
    g->default_value = value_uid;
    if (follow)
        for (SoundLibrary& lib : store.libraries)
            for (SoundNode& sw : lib.nodes) {
                if (sw.type != SoundNodeType::Switch || sw.group_uid != group_uid || sw.default_value.empty()) continue;
                sw.default_value.clear();
                changed = true;
            }
    return changed;
}

// ---- Libraries ----------------------------------------------------------------------------------

bool NewLibrary(SoundStore& store, std::string* uid)
{
    std::string name;
    for (int i = 1;; ++i) {
        name = "Library " + std::to_string(i);
        bool used = false;
        for (const SoundLibrary& l : store.libraries)
            if (SwitchNameKey(l.name) == SwitchNameKey(name)) used = true;
        if (!used) break;
    }
    SoundLibrary lib;
    lib.uid = NewSoundUid();
    lib.name = name;
    SoundNode root;
    root.uid = NewSoundUid();
    root.type = SoundNodeType::Random;
    root.name = name;
    lib.root = root.uid;
    lib.nodes.push_back(root);
    store.libraries.push_back(lib);
    if (uid) *uid = lib.uid;
    return true;
}

bool RenameLibrary(SoundStore& store, const std::string& lib_uid, const std::string& name)
{
    SoundLibrary* lib = FindLibrary(store, lib_uid);
    if (!lib || name.empty() || lib->name == name) return false;
    std::vector<std::string> names;
    for (const SoundLibrary& o : store.libraries)
        if (o.uid != lib_uid) names.push_back(o.name);
    const std::string unique = UniqueName(name, names);
    if (lib->name == unique) return false;
    lib->name = unique;
    return true;
}

bool DeleteLibrary(SoundStore& store, const std::string& lib_uid)
{
    const size_t n = store.libraries.size();
    store.libraries.erase(std::remove_if(store.libraries.begin(), store.libraries.end(),
                                         [&](const SoundLibrary& l) { return l.uid == lib_uid; }),
                          store.libraries.end());
    if (store.libraries.size() == n) return false;
    // 13-15a: a Project / Character assignment of that library goes back to inherit.
    auto unset = [&](std::vector<SoundAssign>& list) {
        for (SoundAssign& a : list)
            if (a.state == LibState::Library && a.library == lib_uid) {
                a.state = LibState::Inherit;
                a.library.clear();
                a.kit = false;
            }
    };
    unset(store.project);
    for (SoundCharacter& c : store.characters) unset(c.assigns);
    return true;
}

int CountMissing(const SoundLibrary& lib, const std::function<bool(const SoundNode&)>& is_missing)
{
    if (!is_missing) return 0;
    int n = 0;
    for (const SoundNode& s : lib.nodes)
        if (s.type == SoundNodeType::Sound && is_missing(s)) ++n;
    return n;
}

}  // namespace rav
