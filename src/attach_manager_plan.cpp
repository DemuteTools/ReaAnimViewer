// SPDX-License-Identifier: MIT
//
// See attach_manager_plan.h (13-68e v5).

#include "attach_manager_plan.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "sound_record.h"       // FindNode, ChildrenOf, FindGroup
#include "sound_spread_plan.h"  // RemoveSubtree, IsSideGroup, SideGroup, SideValueOf

namespace rav {
namespace am {

namespace {

const char kDot[] = " \xC2\xB7 ";

std::string Seconds(double t)
{
    char b[48];
    std::snprintf(b, sizeof(b), "%.2f s", t);
    return b;
}

std::string Plural(int n, const char* one, const char* many)
{
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// A draw's node: its own, or a loop pass's (PassDrawKey "<uid>#p<id>", sound_populate_plan.h).
std::string DrawNode(const std::string& key)
{
    const size_t at = key.rfind("#p");
    if (at == std::string::npos || at == 0 || at + 2 >= key.size()) return key;
    size_t i = at + 2;
    if (key[i] == '-') ++i;
    if (i >= key.size()) return key;
    for (; i < key.size(); ++i)
        if (key[i] < '0' || key[i] > '9') return key;
    return key.substr(0, at);
}

}  // namespace

std::string GuidKey(const std::string& g)
{
    std::string out = g;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool TagListed(const ItemSounds& owner_rec, const std::string& guid)
{
    const std::string key = GuidKey(guid);
    if (key.empty()) return false;
    for (const EventSounds& ev : owner_rec.events)
        for (const SoundPlacement& p : ev.placed)
            if (p.own != Ownership::Deleted && GuidKey(p.item_guid) == key) return true;
    return false;
}

// ---- Apply to ----------------------------------------------------------------------------------------------------

Scope ScopeOf(const SoundStore& store)
{
    return store.attach_scope == 0 ? Scope::Item : store.attach_scope == 2 ? Scope::Project : Scope::Anim;
}

bool SetScope(SoundStore& store, Scope s)
{
    if (ScopeOf(store) == s) return false;
    store.attach_scope = s == Scope::Item ? 0 : s == Scope::Project ? 2 : 1;
    return true;
}

bool IsDeleteUndoName(const std::string& desc, const std::vector<std::string>& localized)
{
    std::string low = desc;
    for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (const char* n : {"remove item", "delete item", "remove selected item", "delete selected item"})
        if (low.find(n) != std::string::npos) return true;
    for (const std::string& l : localized)
        if (!l.empty() && desc.find(l) != std::string::npos) return true;
    return false;
}

const char* ScopeLabel(Scope s)
{
    return s == Scope::Item ? "Item" : s == Scope::Project ? "Project" : "Anim";
}

const char* ScopeWords(Scope s)
{
    return s == Scope::Item ? "this instance" : s == Scope::Project ? "the project" : "this animation";
}

const char* ScopeTip(Scope s)
{
    switch (s) {
    case Scope::Item: return "This instance only";
    case Scope::Anim: return "Every item of this animation (same name), linked copies included";
    case Scope::Project: return "Every instance of these markers in the project";
    }
    return "";
}

// ---- The window's choices ----------------------------------------------------------------------------------------

std::string MarkerKey(const Snapshot& s, const Marker& m)
{
    const std::string anim = m.anim >= 0 && static_cast<size_t>(m.anim) < s.anims.size() ? GuidKey(s.anims[static_cast<size_t>(m.anim)].guid)
                                                                                       : std::string();
    return anim + "|" + m.name + "|" + std::to_string(static_cast<long long>(std::llround(m.pos * 1000.0)));
}

std::vector<int> MarkerChoices(const Snapshot& s)
{
    std::vector<int> order;
    for (size_t i = 0; i < s.markers.size(); ++i) order.push_back(static_cast<int>(i));
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return s.markers[static_cast<size_t>(a)].pos < s.markers[static_cast<size_t>(b)].pos; });
    std::vector<int>      out;
    std::set<std::string> kinds, keys;
    for (int i : order) {  // the time selection's: one per kind and side, its first in time
        const Marker& m = s.markers[static_cast<size_t>(i)];
        if (m.kind_key.empty()) continue;
        const bool in_range = s.has_time_sel && m.pos >= s.ts0 - 1e-9 && m.pos <= s.ts1 + 1e-9;
        if (!in_range) continue;
        if (!kinds.insert(m.kind_key + "|" + std::string(1, m.side ? m.side : '-')).second) continue;
        out.push_back(i);
        keys.insert(MarkerKey(s, m));
    }
    for (int i : order) {  // the clicked labels
        const Marker& m = s.markers[static_cast<size_t>(i)];
        if (!m.clicked || m.kind_key.empty()) continue;
        if (!keys.insert(MarkerKey(s, m)).second) continue;
        out.push_back(i);
    }
    std::stable_sort(out.begin(), out.end(), [&](int a, int b) { return s.markers[static_cast<size_t>(a)].pos < s.markers[static_cast<size_t>(b)].pos; });
    return out;
}

int PreselectMarker(const Snapshot& s, const std::vector<int>& choices, const std::string& pick_key)
{
    if (choices.empty()) return -1;
    if (!pick_key.empty())
        for (int i : choices)
            if (MarkerKey(s, s.markers[static_cast<size_t>(i)]) == pick_key) return i;
    for (int i : choices)
        if (s.markers[static_cast<size_t>(i)].clicked) return i;
    return choices.front();
}

std::string MarkerText(const Marker& m)
{
    return (m.label.empty() ? m.name : m.label) + kDot + Seconds(m.pos);
}

int PileReference(const std::vector<StackRef>& refs, bool has_anchor, double anchor, int* distinct)
{
    std::set<std::string> keys;
    for (const StackRef& r : refs)
        keys.insert(!r.layer.empty() ? "L|" + r.layer
                                     : "E|" + GuidKey(r.owner) + "|" + r.rid + "|" + std::to_string(static_cast<long long>(std::llround(r.t * 1000.0))));
    if (distinct) *distinct = static_cast<int>(keys.size());
    if (keys.size() != 1) return -1;
    int best = 0;
    for (size_t i = 1; i < refs.size() && has_anchor; ++i)
        if (std::fabs(refs[i].pos - anchor) < std::fabs(refs[static_cast<size_t>(best)].pos - anchor)) best = static_cast<int>(i);
    return best;
}

GestureChoice ChooseGesture(const Snapshot& s, const std::vector<int>& choices, int marker, GestureWant want)
{
    GestureChoice c;
    const int ref = PileReference(s.stack_refs, s.has_fresh_anchor, s.fresh_anchor, &c.stack_events);
    if (ref >= 0) c.stack_event = s.stack_refs[static_cast<size_t>(ref)].event;
    c.pile_ok = !s.stack_refs.empty() && s.fresh > 0;
    c.gesture = want == GestureWant::Pile ? Gesture::Pile : want == GestureWant::Attach ? Gesture::Attach
                                                          : (c.pile_ok ? Gesture::Pile : Gesture::Attach);
    if (c.gesture == Gesture::Pile) {
        if (!c.pile_ok) {
            c.reason = "Select an attached instance with the new items.";
            c.from = "Add to pile: no attached instance with new items in the selection.";
        } else if (c.stack_events > 1) {
            c.reason = "Select instances of one layer only with the new items (here " + std::to_string(c.stack_events) + ").";
            c.from = "The selection holds instances of " + std::to_string(c.stack_events) + " different layers or markers.";
        } else {
            c.from = c.stack_event >= 0 ? "From the selected instance." : "From the selected instance (its marker is not read now).";
        }
        return c;
    }
    c.marker = marker;
    std::vector<std::string> seen;
    for (int i : choices) {
        const Marker& m = s.markers[static_cast<size_t>(i)];
        const bool    in_range = s.has_time_sel && m.pos >= s.ts0 - 1e-9 && m.pos <= s.ts1 + 1e-9;
        if (in_range) seen.push_back(m.label.empty() ? m.name : m.label);
    }
    std::string from;
    if (!seen.empty()) {
        from = "Time selection: ";
        for (size_t i = 0; i < seen.size(); ++i) from += (i ? ", " : "") + seen[i];
        from += ".";
    }
    for (int i : choices)
        if (s.markers[static_cast<size_t>(i)].clicked) {
            from += std::string(from.empty() ? "" : " ") + "Clicked label: " + s.markers[static_cast<size_t>(i)].label + ".";
            break;
        }
    if (marker < 0) {
        c.from = s.has_time_sel ? "No RAV marker in the time selection, no marker label clicked."
                                : "No time selection, no marker label clicked.";
        c.reason = s.take_only.empty() ? "Make a time selection over a RAV marker (or click a RAV marker label)." : s.take_only;
        return c;
    }
    c.from = from;
    if (s.sounds <= 0) c.reason = "Select the sound to attach.";
    return c;
}

std::string RunLabel(Gesture g, Scope s)
{
    return std::string(g == Gesture::Pile ? "Add to pile" : "Attach") + kDot + ScopeLabel(s);
}

void GestureInfo(Gesture g, Scope s, const Snapshot& snap, const std::string& kind, char side, bool split,
                 const std::string& target, std::string* bold, std::string* rest)
{
    if (g == Gesture::Pile) {
        *bold = Plural(snap.fresh, "new item", "new items");
        std::string at;
        if (snap.has_pile_offset) {
            const long long ms = std::llround(snap.pile_offset_s * 1000.0);
            at = " at " + std::string(ms >= 0 ? "+" : "") + std::to_string(ms) + " ms";
        }
        *rest = " \xE2\x86\x92 a layer" + at + " on " + (s == Scope::Item ? "this instance" : "every linked instance");
        return;
    }
    *bold = Plural(snap.sounds, "item", "items");
    std::string grp;
    const std::string where = s == Scope::Anim ? "this animation" : "the project";
    if (s == Scope::Item) grp = "on this marker only";
    else if (!side) grp = "every " + kind + " of " + where;
    else if (split) grp = "every " + kind + " " + std::string(1, side) + " of " + where;
    else grp = "every " + kind + " (L and R) of " + where;
    *rest = (snap.piles > 1 ? " in " + Plural(snap.piles, "variation", "variations") : std::string()) + kDot + "onto " + target +
            (s == Scope::Item ? ", " : ", then ") + grp;
}

// ---- The library's layout -----------------------------------------------------------------------------------------

namespace {

bool IsSideSwitch(const SoundStore& store, const SoundNode* n)
{
    if (!n || n->type != SoundNodeType::Switch) return false;
    const SwitchGroup* g = FindGroup(store, n->group_uid);
    return g && IsSideGroup(*g);
}

std::string CaseChild(const SoundNode& sw, const std::string& value)
{
    for (const SwitchCase& c : sw.cases)
        if (c.value_uid == value) return c.child_uid;
    return std::string();
}

// A group's default value (its own when it holds it, else the first).
std::string GroupDefault(const SwitchGroup& g)
{
    for (const SwitchValue& v : g.values)
        if (!g.default_value.empty() && v.uid == g.default_value) return v.uid;
    return g.values.empty() ? std::string() : g.values.front().uid;
}

// A Switch's default value (its own when its group holds it, else the group's).
std::string SwitchDefault(const SwitchGroup& g, const SoundNode& sw)
{
    for (const SwitchValue& v : g.values)
        if (!sw.default_value.empty() && v.uid == sw.default_value) return v.uid;
    return GroupDefault(g);
}

// A node that plays something: present, and not an empty Blend (what a deleted last layer leaves).
bool IsContent(const SoundLibrary& lib, const std::string& uid)
{
    const SoundNode* n = uid.empty() ? nullptr : FindNode(lib, uid);
    return n && !(n->type == SoundNodeType::Blend && ChildrenOf(lib, uid).empty());
}

Group ReadGroup(const SoundStore& store, const SoundLibrary& lib, const Place& at, const std::string& node, const std::string& value,
                bool is_default)
{
    Group g;
    g.value = value;
    g.is_default = is_default;
    g.at = at;
    const SoundNode* n = node.empty() ? nullptr : FindNode(lib, node);
    if (!n) return g;
    g.node = node;
    if (IsSideSwitch(store, n)) {
        const SwitchGroup* sg = FindGroup(store, n->group_uid);
        g.split = true;
        g.pile_l = CaseChild(*n, SideValueOf(*sg, 'L'));
        g.pile_r = CaseChild(*n, SideValueOf(*sg, 'R'));
        if (!IsContent(lib, g.pile_l)) g.pile_l.clear();
        if (!IsContent(lib, g.pile_r)) g.pile_r.clear();
    } else if (IsContent(lib, node)) {
        g.pile = node;
    }
    return g;
}

}  // namespace

bool HasContent(const Group& g)
{
    return !g.pile.empty() || !g.pile_l.empty() || !g.pile_r.empty();
}

bool LegacyLayout(const SoundStore& store, const SoundLibrary& lib, std::string* why)
{
    const Layout          lay = ReadLayout(store, lib);
    std::set<std::string> side_ok;  // the Side Switches a v5 Group holds
    for (const Group& g : lay.groups)
        if (g.split) side_ok.insert(g.node);
    for (const SoundNode& n : lib.nodes) {
        if (n.type != SoundNodeType::Switch) continue;
        const SwitchGroup* sg = FindGroup(store, n.group_uid);
        if (!sg || !sg->rav_owned) continue;
        bool legacy = false;
        if (IsSideGroup(*sg)) legacy = !side_ok.count(n.uid) || CaseChild(n, SideValueOf(*sg, 'L')).empty() || CaseChild(n, SideValueOf(*sg, 'R')).empty();
        else if (!IsAnimGroup(*sg)) legacy = true;  // v3's "<kind> layers"
        else legacy = n.uid != lay.anim_switch;
        if (legacy) {
            if (why)
                *why = "The " + lib.name + " library was laid out by an earlier test version of the Attach manager: delete it in the "
                                           "Library view and attach again";
            return true;
        }
    }
    return false;
}

bool IsAnimGroup(const SwitchGroup& g)
{
    const size_t n = std::strlen(kAnimGroupSuffix);
    return g.rav_owned && g.name.size() > n && g.name.compare(g.name.size() - n, n, kAnimGroupSuffix) == 0;
}

Layout ReadLayout(const SoundStore& store, const SoundLibrary& lib)
{
    Layout           lay;
    const SoundNode* root = lib.root.empty() ? nullptr : FindNode(lib, lib.root);
    const SwitchGroup* ag = root && root->type == SoundNodeType::Switch ? FindGroup(store, root->group_uid) : nullptr;
    if (root && ag && IsAnimGroup(*ag)) {
        lay.anim_switch = root->uid;
        lay.anim_group = ag->uid;
        lay.default_value = SwitchDefault(*ag, *root);
        lay.groups.push_back(ReadGroup(store, lib, Place{root->uid, lay.default_value}, CaseChild(*root, lay.default_value),
                                       lay.default_value, true));
        for (const SwitchCase& c : root->cases)
            if (c.value_uid != lay.default_value && FindNode(lib, c.child_uid))
                lay.groups.push_back(ReadGroup(store, lib, Place{root->uid, c.value_uid}, c.child_uid, c.value_uid, false));
        return lay;
    }
    lay.groups.push_back(ReadGroup(store, lib, Place{}, lib.root, std::string(), true));
    return lay;
}

int GroupOf(const Layout& lay, const std::vector<ValueSet>& item_values)
{
    if (lay.groups.empty()) return -1;
    if (lay.anim_switch.empty()) return 0;
    for (const ValueSet& v : item_values)
        if (v.group_uid == lay.anim_group)
            for (size_t i = 0; i < lay.groups.size(); ++i)
                if (lay.groups[i].value == v.value_uid) return static_cast<int>(i);
    return 0;  // the default
}

std::string PileOf(const Group& g, char side)
{
    if (!g.split) return g.pile;
    return side == 'R' ? g.pile_r : g.pile_l;  // an event with no side plays the Side Switch's default, L
}

std::vector<std::string> PileLayers(const SoundLibrary& lib, const std::string& pile)
{
    std::vector<std::string> out;
    const SoundNode*         p = pile.empty() ? nullptr : FindNode(lib, pile);
    if (!p) return out;
    if (p->type != SoundNodeType::Blend) return {pile};
    for (const SoundNode* c : ChildrenOf(lib, pile)) out.push_back(c->uid);
    return out;
}

LayerAt LayerOfNode(const SoundLibrary& lib, const Layout& lay, const std::string& node)
{
    LayerAt                                   out;
    std::map<std::string, std::pair<int, char>> piles;
    for (size_t i = 0; i < lay.groups.size(); ++i) {
        const Group& g = lay.groups[i];
        if (!g.split && !g.pile.empty()) piles[g.pile] = {static_cast<int>(i), 0};
        if (g.split && !g.pile_l.empty()) piles[g.pile_l] = {static_cast<int>(i), 'L'};
        if (g.split && !g.pile_r.empty()) piles[g.pile_r] = {static_cast<int>(i), 'R'};
    }
    std::string prev, cur = node;
    for (size_t guard = 0; !cur.empty() && guard <= lib.nodes.size(); ++guard) {
        const SoundNode* n = FindNode(lib, cur);
        if (!n) break;
        const auto hit = piles.find(cur);
        if (hit != piles.end()) {
            out.group = hit->second.first;
            out.slot = hit->second.second;
            out.pile = cur;
            out.layer = prev.empty() || n->type != SoundNodeType::Blend ? cur : prev;
            return out;
        }
        prev = cur;
        cur = n->parent;
    }
    return out;
}

std::string PlayedSoundOf(const SoundStore&, const SoundLibrary& lib, const Layout& lay, const std::vector<ValueSet>& item_values,
                          char side, const std::string& source_guid)
{
    const int gi = GroupOf(lay, item_values);
    if (gi < 0 || source_guid.empty()) return std::string();
    const std::string     pile = PileOf(lay.groups[static_cast<size_t>(gi)], side);
    std::set<std::string> in = {pile};
    if (pile.empty()) return std::string();
    for (bool grew = true; grew;) {
        grew = false;
        for (const SoundNode& n : lib.nodes)
            if (!in.count(n.uid) && in.count(n.parent)) {
                in.insert(n.uid);
                grew = true;
            }
    }
    const std::string key = GuidKey(source_guid);
    for (const SoundNode& n : lib.nodes)
        if (in.count(n.uid) && n.type == SoundNodeType::Sound && GuidKey(n.sound.source.item_guid) == key) return n.uid;
    return std::string();
}

std::string LayerSig(const SoundLibrary& lib, const std::string& layer)
{
    std::set<std::string> in = {layer}, src;
    if (!FindNode(lib, layer)) return std::string();
    for (bool grew = true; grew;) {
        grew = false;
        for (const SoundNode& n : lib.nodes)
            if (!in.count(n.uid) && in.count(n.parent)) {
                in.insert(n.uid);
                grew = true;
            }
    }
    for (const SoundNode& n : lib.nodes)
        if (in.count(n.uid) && n.type == SoundNodeType::Sound && !n.sound.source.item_guid.empty()) src.insert(GuidKey(n.sound.source.item_guid));
    std::string out;
    for (const std::string& s : src) out += (out.empty() ? "" : ",") + s;
    return out;
}

// ---- The library edits --------------------------------------------------------------------------------------------

namespace {

std::string ChildAt(const SoundLibrary& lib, const Place& p)
{
    if (p.sw.empty()) return FindNode(lib, lib.root) ? lib.root : std::string();
    const SoundNode* sw = FindNode(lib, p.sw);
    if (!sw) return std::string();
    const std::string c = CaseChild(*sw, p.value);
    return FindNode(lib, c) ? c : std::string();
}

void SetAt(SoundLibrary& lib, const Place& p, const std::string& child)
{
    if (p.sw.empty()) {
        lib.root = child;
        return;
    }
    SoundNode* sw = FindNode(lib, p.sw);
    if (!sw) return;
    for (SwitchCase& c : sw->cases)
        if (c.value_uid == p.value) {
            c.child_uid = child;
            return;
        }
    SwitchCase c;
    c.value_uid = p.value;
    c.child_uid = child;
    sw->cases.push_back(c);
}

// `nodes` (nodes[0] their top) copied with new uids: parents, cases and lists inside them renamed. `map` gets old -> new.
std::vector<SoundNode> CopyList(const std::vector<SoundNode>& nodes, const std::function<std::string()>& new_uid,
                                std::map<std::string, std::string>* map)
{
    std::map<std::string, std::string> m;
    for (const SoundNode& n : nodes) m[n.uid] = new_uid();
    std::vector<SoundNode> out;
    for (const SoundNode& n : nodes) {
        SoundNode c = n;
        c.uid = m[n.uid];
        const auto p = m.find(n.parent);
        if (p != m.end()) c.parent = p->second;
        for (SwitchCase& sc : c.cases) {
            const auto h = m.find(sc.child_uid);
            if (h != m.end()) sc.child_uid = h->second;
        }
        for (std::string& u : c.unassigned) {
            const auto h = m.find(u);
            if (h != m.end()) u = h->second;
        }
        out.push_back(c);
    }
    if (map)
        for (const auto& kv : m) (*map)[kv.first] = kv.second;
    return out;
}

// The subtree under `uid` (itself first), in the library's line order.
std::vector<SoundNode> Subtree(const SoundLibrary& lib, const std::string& uid)
{
    std::vector<SoundNode> out;
    const SoundNode*       top = FindNode(lib, uid);
    if (!top) return out;
    std::set<std::string> in = {uid};
    for (bool grew = true; grew;) {
        grew = false;
        for (const SoundNode& n : lib.nodes)
            if (!in.count(n.uid) && in.count(n.parent)) {
                in.insert(n.uid);
                grew = true;
            }
    }
    out.push_back(*top);
    for (const SoundNode& n : lib.nodes)
        if (n.uid != uid && in.count(n.uid)) out.push_back(n);
    return out;
}

// `nodes` put at `p` (nodes[0] its child): what was there goes with everything under it.
void PutAt(SoundLibrary& lib, const Place& p, std::vector<SoundNode> nodes)
{
    if (nodes.empty()) return;
    const std::string old = ChildAt(lib, p);
    if (!old.empty()) RemoveSubtree(lib, old);
    nodes[0].parent = p.sw;
    const std::string top = nodes[0].uid;
    for (SoundNode& n : nodes) lib.nodes.push_back(std::move(n));
    SetAt(lib, p, top);
}

SoundLibrary* LibOf(SoundStore& store, const std::string& uid)
{
    for (SoundLibrary& l : store.libraries)
        if (l.uid == uid) return &l;
    return nullptr;
}

// The library's AnimSwitch at its root, made when absent: its group (RAV-owned "<kind> animations", one left by an
// earlier Switch reused) with a default value "Other animations" playing what the root played. Its uid.
std::string EnsureAnimSwitch(SoundStore& store, const std::string& lib_uid, const std::string& kind,
                             const std::function<std::string()>& new_uid)
{
    SoundLibrary* lib = LibOf(store, lib_uid);
    if (!lib) return std::string();
    const Layout lay = ReadLayout(store, *lib);
    if (!lay.anim_switch.empty()) return lay.anim_switch;
    const std::string gname = (kind.empty() ? lib->name : kind) + kAnimGroupSuffix;
    SwitchGroup*      g = nullptr;
    for (SwitchGroup& og : store.groups)
        if (!g && og.rav_owned && og.name == gname) g = &og;
    if (!g) {
        SwitchGroup ng;
        ng.uid = new_uid();
        ng.name = gname;
        ng.rav_owned = true;
        SwitchValue other;
        other.uid = new_uid();
        other.name = "Other animations";
        ng.values.push_back(other);
        ng.default_value = other.uid;
        store.groups.push_back(ng);
        g = &store.groups.back();
    }
    if (g->values.empty()) {  // a damaged one gets its default back
        SwitchValue other;
        other.uid = new_uid();
        other.name = "Other animations";
        g->values.push_back(other);
        g->default_value = other.uid;
    }
    SoundNode sw;
    sw.uid = new_uid();
    sw.type = SoundNodeType::Switch;
    sw.name = g->name;
    sw.group_uid = g->uid;
    sw.layer_keep_offset = true;
    const std::string def = GroupDefault(*g);
    if (SoundNode* old = FindNode(*lib, lib->root)) {
        old->parent = sw.uid;
        SwitchCase c;
        c.value_uid = def;
        c.child_uid = old->uid;
        sw.cases.push_back(c);
    }
    lib->nodes.insert(lib->nodes.begin(), sw);
    lib->root = sw.uid;
    return sw.uid;
}

// The animation's value of the AnimSwitch: the one its items hold, else the one named after it, else a new one ("Walk",
// "Walk 2"...); never the default, never one held outside its pool.
std::string PoolValue(SwitchGroup& g, const std::string& def, const PoolKey& pool, const std::function<std::string()>& new_uid)
{
    for (const ValueSet& v : pool.item_values)
        if (v.group_uid == g.uid && v.value_uid != def && !pool.held_elsewhere.count(v.value_uid))
            for (const SwitchValue& x : g.values)
                if (x.uid == v.value_uid) return x.uid;
    for (const SwitchValue& x : g.values)
        if (x.uid != def && x.name == pool.anim_name && !pool.held_elsewhere.count(x.uid)) return x.uid;
    SwitchValue v;
    v.uid = new_uid();
    const std::string base = pool.anim_name.empty() ? std::string("Animation") : pool.anim_name;
    v.name = base;
    for (int k = 2;; ++k) {
        const bool used = std::any_of(g.values.begin(), g.values.end(), [&](const SwitchValue& x) { return x.name == v.name; });
        if (!used) break;
        v.name = base + " " + std::to_string(k);
    }
    g.values.push_back(v);
    return v.uid;
}

// Anim: the animation's own Group (its AnimSwitch case), copied from the default's content the first time (out->remap_pool).
// Its place; an empty `sw` on failure.
Place OwnGroup(SoundStore& store, const std::string& lib_uid, const PoolKey& pool, const std::function<std::string()>& new_uid,
               EditOut* out)
{
    const std::string sw_uid = EnsureAnimSwitch(store, lib_uid, pool.kind_name, new_uid);
    SoundLibrary*     lib = LibOf(store, lib_uid);
    SoundNode*        sw = lib ? FindNode(*lib, sw_uid) : nullptr;
    SwitchGroup*      g = sw ? FindGroup(store, sw->group_uid) : nullptr;
    if (!sw || !g) return Place{};
    const std::string def = SwitchDefault(*g, *sw);
    const std::string value = PoolValue(*g, def, pool, new_uid);
    out->anim_value.group_uid = g->uid;
    out->anim_value.value_uid = value;
    const Place at{sw_uid, value};
    if (!ChildAt(*lib, at).empty()) return at;
    const std::string def_child = ChildAt(*lib, Place{sw_uid, def});
    if (!def_child.empty()) PutAt(*lib, at, CopyList(Subtree(*lib, def_child), new_uid, &out->remap_pool));
    return at;
}

// Every Group's place (the default first); `with_default`: an AnimSwitch's default with no case is listed too.
std::vector<Place> GroupPlaces(const SoundStore& store, const SoundLibrary& lib, bool with_default)
{
    std::vector<Place> out;
    const Layout       lay = ReadLayout(store, lib);
    for (const Group& g : lay.groups)
        if (with_default || !g.node.empty()) out.push_back(g.at);
    return out;
}

// Where a Group's content for `side` goes: a split Group's side case, else the Group's place.
Place SlotPlace(SoundStore& store, const SoundLibrary& lib, const Place& gp, char side, char* slot)
{
    const std::string child = ChildAt(lib, gp);
    const SoundNode*  n = child.empty() ? nullptr : FindNode(lib, child);
    *slot = 0;
    if (!IsSideSwitch(store, n)) return gp;
    const SwitchGroup* sg = FindGroup(store, n->group_uid);
    *slot = side == 'R' ? 'R' : 'L';
    return Place{child, SideValueOf(*sg, *slot)};
}

// The places one gesture changes: the animation's own Group (Anim) or every Group (Project). Empty on failure.
std::vector<Place> ScopePlaces(SoundStore& store, const std::string& lib_uid, Scope scope, const PoolKey& pool, bool with_default,
                               const std::function<std::string()>& new_uid, EditOut* out)
{
    if (scope == Scope::Anim) {
        const Place p = OwnGroup(store, lib_uid, pool, new_uid, out);
        if (p.sw.empty()) return {};
        return {p};
    }
    const SoundLibrary* lib = LibOf(store, lib_uid);
    return lib ? GroupPlaces(store, *lib, with_default) : std::vector<Place>();
}

// A Pile as a Blend: itself when it is one, else wrapped in a new Blend at its place (the old content its first layer).
std::string PileBlend(SoundLibrary& lib, const Place& at, const std::string& pile, const std::string& blend_uid)
{
    SoundNode* p = FindNode(lib, pile);
    if (!p) return std::string();
    if (p->type == SoundNodeType::Blend) return pile;
    SoundNode b;
    b.uid = blend_uid;
    b.type = SoundNodeType::Blend;
    b.name = p->name.empty() ? lib.name : p->name;
    b.parent = at.sw;
    b.layer_keep_offset = true;
    p->parent = b.uid;
    p->offset_ms = 0.0;
    p->layer_keep_offset = true;
    size_t i = 0;
    while (i < lib.nodes.size() && lib.nodes[i].uid != pile) ++i;
    lib.nodes.insert(lib.nodes.begin() + static_cast<long>(i), b);
    SetAt(lib, at, blend_uid);
    return blend_uid;
}

// A case left with nothing plays nothing (an empty Blend), never its Switch's default (what an Anim Switch's other
// animations, or a Side Switch's other side, play).
void EmptyAt(SoundLibrary& lib, const Place& p, const std::function<std::string()>& new_uid)
{
    if (p.sw.empty() || !ChildAt(lib, p).empty()) return;
    SoundNode b;
    b.uid = new_uid();
    b.type = SoundNodeType::Blend;
    b.name = "Nothing";
    PutAt(lib, p, {b});
}

bool SlotsMatch(char a, char b)
{
    return a == 0 || b == 0 || a == b;
}

}  // namespace

static EditOut AttachContentImpl(SoundStore& store, const std::string& lib_uid, Scope scope, const PoolKey& pool, char side,
                      std::vector<SoundNode> nodes, const std::function<std::string()>& new_uid)
{
    EditOut out;
    if (nodes.empty() || !LibOf(store, lib_uid) || scope == Scope::Item) {
        out.why = "Nothing to attach";
        return out;
    }
    const std::vector<Place> places = ScopePlaces(store, lib_uid, scope, pool, true, new_uid, &out);
    if (places.empty()) {
        out.why = "The library could not be read";
        return out;
    }
    bool first = true;
    for (const Place& gp : places) {
        SoundLibrary* lib = LibOf(store, lib_uid);
        char          slot = 0;
        const Place   sp = SlotPlace(store, *lib, gp, side, &slot);
        PutAt(*LibOf(store, lib_uid), sp, first ? nodes : CopyList(nodes, new_uid, nullptr));
        first = false;
        out.slots.insert(slot);
        ++out.groups;
    }
    out.ok = true;
    return out;
}

static EditOut AddPileLayerImpl(SoundStore& store, const std::string& lib_uid, Scope scope, const PoolKey& pool, char side,
                     std::vector<SoundNode> layer, double offset_ms, const std::function<std::string()>& new_uid)
{
    EditOut out;
    if (layer.empty() || !LibOf(store, lib_uid) || scope == Scope::Item) {
        out.why = "Nothing to add";
        return out;
    }
    const std::vector<Place> places = ScopePlaces(store, lib_uid, scope, pool, false, new_uid, &out);
    bool first = true;
    for (const Place& gp : places) {
        SoundLibrary*     lib = LibOf(store, lib_uid);
        char              slot = 0;
        const Place       sp = SlotPlace(store, *lib, gp, side, &slot);
        const std::string pile = ChildAt(*lib, sp);
        if (!IsContent(*lib, pile)) continue;  // nothing attached there: no layer alone
        std::vector<SoundNode> l = first ? layer : CopyList(layer, new_uid, nullptr);
        first = false;
        const std::string blend = PileBlend(*lib, sp, pile, new_uid());
        if (blend.empty()) continue;
        l[0].parent = blend;
        l[0].offset_ms = offset_ms;
        l[0].layer_keep_offset = true;
        l[0].name = "Layer " + std::to_string(ChildrenOf(*lib, blend).size() + 1);
        for (SoundNode& n : l) lib->nodes.push_back(std::move(n));
        out.slots.insert(slot);
        ++out.groups;
    }
    out.ok = out.groups > 0;
    if (!out.ok) out.why = "Nothing is attached on these markers yet";
    return out;
}

static EditOut SeparateSidesImpl(SoundStore& store, const std::string& lib_uid, Scope scope, const PoolKey& pool,
                      const std::function<std::string()>& new_uid)
{
    EditOut out;
    if (!LibOf(store, lib_uid) || scope == Scope::Item) {
        out.why = "Separate L and R works on Anim or Project";
        return out;
    }
    const std::vector<Place> places = ScopePlaces(store, lib_uid, scope, pool, false, new_uid, &out);
    const SwitchGroup*       side_g = SideGroup(store, new_uid);
    const std::string        vl = SideValueOf(*side_g, 'L'), vr = SideValueOf(*side_g, 'R');
    const std::string        side_uid = side_g->uid, side_name = side_g->name;
    bool                     already = false;
    for (const Place& gp : places) {
        SoundLibrary*     lib = LibOf(store, lib_uid);
        const std::string child = ChildAt(*lib, gp);
        if (IsSideSwitch(store, FindNode(*lib, child))) {
            already = true;
            continue;
        }
        if (!IsContent(*lib, child)) continue;
        std::vector<SoundNode> copy = CopyList(Subtree(*lib, child), new_uid, &out.remap_side);
        SoundNode              ss;
        ss.uid = new_uid();
        ss.parent = gp.sw;
        ss.type = SoundNodeType::Switch;
        ss.name = side_name;
        ss.group_uid = side_uid;
        ss.layer_keep_offset = true;
        SwitchCase cl;
        cl.value_uid = vl;
        cl.child_uid = child;
        ss.cases.push_back(cl);
        FindNode(*lib, child)->parent = ss.uid;
        size_t i = 0;
        while (i < lib->nodes.size() && lib->nodes[i].uid != child) ++i;
        lib->nodes.insert(lib->nodes.begin() + static_cast<long>(i), ss);
        SetAt(*lib, gp, ss.uid);
        PutAt(*lib, Place{ss.uid, vr}, copy);
        ++out.groups;
    }
    out.ok = out.groups > 0;
    if (!out.ok) out.why = already ? "L and R are separate already" : "Nothing is attached here yet";
    return out;
}

static EditOut MergeSidesImpl(SoundStore& store, const std::string& lib_uid, Scope scope, const PoolKey& pool, char keep,
                   const std::function<std::string()>& new_uid)
{
    EditOut out;
    if (!LibOf(store, lib_uid) || scope == Scope::Item || (keep != 'L' && keep != 'R')) {
        out.why = "Merge L and R works on Anim or Project, from an L or R marker";
        return out;
    }
    const std::vector<Place> places = ScopePlaces(store, lib_uid, scope, pool, false, new_uid, &out);
    for (const Place& gp : places) {
        SoundLibrary*     lib = LibOf(store, lib_uid);
        const std::string child = ChildAt(*lib, gp);
        const SoundNode*  ss = child.empty() ? nullptr : FindNode(*lib, child);
        if (!IsSideSwitch(store, ss)) continue;
        const SwitchGroup* sg = FindGroup(store, ss->group_uid);
        const std::string  kept = CaseChild(*ss, SideValueOf(*sg, keep));
        if (SoundNode* k = FindNode(*lib, kept)) k->parent = gp.sw;
        RemoveSubtree(*lib, child);  // the Side Switch and the other side's Pile
        if (FindNode(*lib, kept)) SetAt(*lib, gp, kept);
        else if (!(gp.value == ReadLayout(store, *lib).default_value)) EmptyAt(*lib, gp, new_uid);
        ++out.groups;
    }
    out.ok = out.groups > 0;
    if (out.ok) out.dropped_side = keep == 'L' ? 'R' : 'L';
    else out.why = "L and R play the same sound already";
    return out;
}

static EditOut RemoveLayerImpl(SoundStore& store, const std::string& lib_uid, Scope scope, const PoolKey& pool, const std::string& layer,
                    char slot, const std::function<std::string()>& new_uid)
{
    EditOut out;
    SoundLibrary* lib0 = LibOf(store, lib_uid);
    if (!lib0 || scope == Scope::Item || layer.empty()) {
        out.why = "Nothing to remove";
        return out;
    }
    const std::string sig = LayerSig(*lib0, layer);
    std::vector<Place> places = ScopePlaces(store, lib_uid, scope, pool, false, new_uid, &out);
    for (const Place& gp : places) {
        SoundLibrary* lib = LibOf(store, lib_uid);
        const Group   g = ReadGroup(store, *lib, gp, ChildAt(*lib, gp), gp.value, false);
        struct Slot {
            Place       at;
            char        side;
            std::string pile;
        };
        std::vector<Slot> slots;
        if (g.split) {
            const SoundNode*   ss = FindNode(*lib, g.node);
            const SwitchGroup* sg = ss ? FindGroup(store, ss->group_uid) : nullptr;
            if (sg) slots = {{Place{g.node, SideValueOf(*sg, 'L')}, 'L', g.pile_l}, {Place{g.node, SideValueOf(*sg, 'R')}, 'R', g.pile_r}};
        } else {
            slots = {{gp, 0, g.pile}};
        }
        for (const Slot& s : slots) {
            if (s.pile.empty()) continue;
            std::vector<std::string> targets;  // every match: several layers of one Pile may be copies of it
            for (const std::string& l : PileLayers(*lib, s.pile)) {
                const auto mapped = out.remap_pool.find(layer);
                const bool same = l == layer || (mapped != out.remap_pool.end() && l == mapped->second);
                const bool copy = scope == Scope::Project && !sig.empty() && LayerSig(*lib, l) == sig && SlotsMatch(slot, s.side);
                if (same || copy) targets.push_back(l);
            }
            if (targets.empty()) continue;
            for (const std::string& target : targets) RemoveSubtree(*lib, target);
            if (FindNode(*lib, s.pile) && ChildrenOf(*lib, s.pile).empty()) RemoveSubtree(*lib, s.pile);
            // An animation's own case or a side left empty plays nothing (not the default's sound).
            const bool anim_default = !gp.sw.empty() && gp.value == ReadLayout(store, *lib).default_value;
            if (!(s.at.sw == gp.sw && anim_default)) EmptyAt(*lib, s.at, new_uid);
            out.slots.insert(s.side);
            ++out.groups;
        }
    }
    out.ok = out.groups > 0;
    if (!out.ok) out.why = "That layer is no longer in the library";
    return out;
}

namespace {

// Every edit: refused on a library an earlier version laid out; made on a copy of the store, kept only when it worked.
template <class F>
EditOut Trial(SoundStore& store, const std::string& lib_uid, F edit)
{
    EditOut out;
    if (const SoundLibrary* lib = LibOf(store, lib_uid))
        if (LegacyLayout(store, *lib, &out.why)) return out;
    SoundStore trial = store;
    out = edit(trial);
    if (out.ok) store = std::move(trial);
    return out;
}

}  // namespace

EditOut AttachContent(SoundStore& store, const std::string& lib_uid, Scope scope, const PoolKey& pool, char side,
                      std::vector<SoundNode> nodes, const std::function<std::string()>& new_uid)
{
    return Trial(store, lib_uid, [&](SoundStore& st) { return AttachContentImpl(st, lib_uid, scope, pool, side, nodes, new_uid); });
}

EditOut AddPileLayer(SoundStore& store, const std::string& lib_uid, Scope scope, const PoolKey& pool, char side,
                     std::vector<SoundNode> layer, double offset_ms, const std::function<std::string()>& new_uid)
{
    return Trial(store, lib_uid, [&](SoundStore& st) { return AddPileLayerImpl(st, lib_uid, scope, pool, side, layer, offset_ms, new_uid); });
}

EditOut SeparateSides(SoundStore& store, const std::string& lib_uid, Scope scope, const PoolKey& pool,
                      const std::function<std::string()>& new_uid)
{
    return Trial(store, lib_uid, [&](SoundStore& st) { return SeparateSidesImpl(st, lib_uid, scope, pool, new_uid); });
}

EditOut MergeSides(SoundStore& store, const std::string& lib_uid, Scope scope, const PoolKey& pool, char keep,
                   const std::function<std::string()>& new_uid)
{
    return Trial(store, lib_uid, [&](SoundStore& st) { return MergeSidesImpl(st, lib_uid, scope, pool, keep, new_uid); });
}

EditOut RemoveLayer(SoundStore& store, const std::string& lib_uid, Scope scope, const PoolKey& pool, const std::string& layer,
                    char slot, const std::function<std::string()>& new_uid)
{
    return Trial(store, lib_uid, [&](SoundStore& st) { return RemoveLayerImpl(st, lib_uid, scope, pool, layer, slot, new_uid); });
}

bool RemapEventNodes(EventSounds& ev, const std::map<std::string, std::string>& uids)
{
    bool changed = false;
    for (SoundDraw& d : ev.draws) {
        const std::string node = DrawNode(d.node);
        const auto        hit = uids.find(node);
        if (hit == uids.end()) continue;
        d.node = hit->second + d.node.substr(node.size());  // a loop pass's key keeps its pass
        changed = true;
    }
    for (SoundPlacement& p : ev.placed) {
        const auto hit = uids.find(p.node);
        if (p.node.empty() || hit == uids.end()) continue;
        p.node = hit->second;
        changed = true;
    }
    return changed;
}

bool RemapTagNodes(PlacedTag& tag, const std::map<std::string, std::string>& uids)
{
    bool changed = false;
    const auto hit = uids.find(tag.node);
    if (!tag.node.empty() && hit != uids.end()) {
        tag.node = hit->second;
        changed = true;
    }
    for (PlacedTag::NodePrint& np : tag.lib) {
        const auto h = uids.find(np.node);
        if (h == uids.end()) continue;
        np.node = h->second;
        changed = true;
    }
    return changed;
}

// ---- The L and R card ---------------------------------------------------------------------------------------------

LrCard LrCardOf(const LrInput& in, Scope scope)
{
    LrCard c;
    c.button = "Separate L and R";
    c.pill = "\xE2\x80\x94";
    if (!in.has_marker) {
        c.text = "Pick a marker.";
        c.tip = "Pick a marker first";
        return c;
    }
    if (!in.marker_read) {
        c.text = "The selected instance's marker is not read now.";
        c.tip = "Select the instance again, or pick a marker";
        return c;
    }
    if (!in.legacy.empty()) {
        c.text = in.legacy + ".";
        c.tip = in.legacy;
        return c;
    }
    const bool has = in.group && HasContent(*in.group);
    if (!has && !(scope == Scope::Project && (in.any_together || in.any_split))) {
        c.text = "Nothing attached on " + in.kind + " here yet.";
        c.tip = "Attach a sound first";
        return c;
    }
    const bool split = has && in.group->split;
    if (has) {
        c.text = in.kind + (split ? ": L and R are two groups." : ": L and R play the same sound.");
        c.pill = split ? "Separate" : "Together";
    } else {
        c.text = "Nothing attached on " + in.kind + " in this animation.";
    }
    // Project: Separate while any Group is together, else Merge; Anim: the marker's animation's Group.
    c.merge = scope == Scope::Project ? !in.any_together && in.any_split : split;
    const std::string side(1, in.side ? in.side : 'L');
    if (c.merge) c.button = "Merge L and R (keep the " + side + " sound)";
    if (scope == Scope::Item) c.tip = "Anim or Project: L and R are groups of instances";
    else if (!in.side) c.tip = "This marker kind has no side";
    c.enabled = c.tip.empty();
    return c;
}

// ---- Linked instances -------------------------------------------------------------------------------------------

std::vector<int> Linked(const std::vector<Instance>& all, int i, Scope scope)
{
    std::vector<int> out;
    if (i < 0 || static_cast<size_t>(i) >= all.size()) return out;
    out.push_back(i);
    const Instance& a = all[static_cast<size_t>(i)];
    if (scope == Scope::Item || a.layer.empty() || a.lib.empty()) return out;
    for (size_t j = 0; j < all.size(); ++j) {
        if (static_cast<int>(j) == i) continue;
        const Instance& b = all[j];
        if (b.lib != a.lib || b.layer.empty()) continue;
        bool hit = false;
        if (scope == Scope::Anim) hit = b.pool == a.pool && b.layer == a.layer;
        else hit = b.layer == a.layer || (!a.sig.empty() && b.sig == a.sig && SlotsMatch(a.slot, b.slot));
        if (hit) out.push_back(static_cast<int>(j));
    }
    return out;
}

std::vector<std::string> ExpandSelection(const std::vector<Instance>& all, const std::set<std::string>& now, Scope scope, bool ctrl)
{
    std::vector<std::string> out;
    if (scope == Scope::Item || ctrl || now.size() != 1) return out;
    const std::string g = GuidKey(*now.begin());
    for (size_t i = 0; i < all.size(); ++i)
        if (all[i].guid == g) {
            const std::vector<int> l = Linked(all, static_cast<int>(i), scope);
            if (l.size() < 2) return out;
            for (int k : l) out.push_back(all[static_cast<size_t>(k)].guid);
            return out;
        }
    return out;
}

DeletePlan PlanPropagatedDelete(const std::vector<Instance>& before, const std::set<std::string>& gone, Scope scope)
{
    DeletePlan p;
    if (scope == Scope::Item) return p;
    std::set<std::string> keys, del;
    for (size_t i = 0; i < before.size(); ++i) {
        const Instance& a = before[i];
        if (!gone.count(a.guid) || a.layer.empty()) continue;
        const std::string key = a.lib + "|" + (scope == Scope::Anim ? a.pool + "|" + a.layer
                                                                    : (a.sig.empty() ? a.layer : a.sig) + "|" + std::string(1, a.slot ? a.slot : '-'));
        for (int j : Linked(before, static_cast<int>(i), scope)) {
            const std::string& g = before[static_cast<size_t>(j)].guid;
            if (!gone.count(g) && del.insert(g).second) p.delete_guids.push_back(g);
        }
        if (!keys.insert(key).second) continue;
        LayerRemoval r;
        r.lib = a.lib;
        r.owner = a.owner;
        r.layer = a.layer;
        r.slot = a.slot;
        p.removals.push_back(r);
    }
    return p;
}

bool SameLook(const Look& a, const Look& b)
{
    auto eq = [](double x, double y) { return std::fabs(x - y) <= 1e-9; };
    return eq(a.vol, b.vol) && eq(a.fade_in, b.fade_in) && eq(a.fade_out, b.fade_out) && a.in_shape == b.in_shape &&
           a.out_shape == b.out_shape && eq(a.in_dir, b.in_dir) && eq(a.out_dir, b.out_dir);
}

std::vector<LookWrite> PlanPropagatedLooks(const std::vector<Instance>& all, const std::map<std::string, Look>& before,
                                           const std::map<std::string, Look>& now, Scope scope)
{
    std::vector<LookWrite> out;
    if (scope == Scope::Item) return out;
    auto eq = [](double x, double y) { return std::fabs(x - y) <= 1e-9; };
    std::set<std::string> changed, written;
    for (const Instance& a : all) {
        const auto b = before.find(a.guid), n = now.find(a.guid);
        if (b != before.end() && n != now.end() && !SameLook(b->second, n->second)) changed.insert(a.guid);
    }
    for (size_t i = 0; i < all.size(); ++i) {
        if (!changed.count(all[i].guid)) continue;
        const Look& was = before.at(all[i].guid);
        const Look& is = now.at(all[i].guid);
        const bool  vol = !eq(was.vol, is.vol);
        const bool  fin = !eq(was.fade_in, is.fade_in) || was.in_shape != is.in_shape || !eq(was.in_dir, is.in_dir);
        const bool  fout = !eq(was.fade_out, is.fade_out) || was.out_shape != is.out_shape || !eq(was.out_dir, is.out_dir);
        for (int j : Linked(all, static_cast<int>(i), scope)) {
            const std::string& g = all[static_cast<size_t>(j)].guid;
            const auto         cur = now.find(g);
            if (changed.count(g) || written.count(g) || cur == now.end()) continue;
            Look l = cur->second;
            if (vol) l.vol = was.vol > 1e-12 ? l.vol * (is.vol / was.vol) : is.vol;
            if (fin) {
                l.fade_in = is.fade_in;
                l.in_shape = is.in_shape;
                l.in_dir = is.in_dir;
            }
            if (fout) {
                l.fade_out = is.fade_out;
                l.out_shape = is.out_shape;
                l.out_dir = is.out_dir;
            }
            if (SameLook(l, cur->second)) continue;
            written.insert(g);
            out.push_back({g, l});
        }
    }
    return out;
}

std::vector<MoveBack> PlanMoveBack(const std::set<std::string>& expanded, const std::string& dragged,
                                   const std::map<std::string, double>& before, const std::map<std::string, double>& now)
{
    std::vector<MoveBack> out;
    std::vector<std::string> moved;
    double                   delta = 0.0;
    for (const auto& kv : now) {
        const auto b = before.find(kv.first);
        if (b == before.end() || std::fabs(kv.second - b->second) <= 1e-9) continue;
        const double d = kv.second - b->second;
        if (!expanded.count(kv.first)) return {};  // something else moved too: not ours
        if (!moved.empty() && std::fabs(d - delta) > 1e-6) return {};  // moved apart: not one drag
        delta = d;
        moved.push_back(kv.first);
    }
    if (moved.size() < 2 || std::find(moved.begin(), moved.end(), dragged) == moved.end()) return out;
    for (const std::string& g : moved)
        if (g != dragged) out.push_back({g, before.at(g)});
    return out;
}

WatchOut DecideWatch(const WatchSnap& last, const WatchSnap& now, const WatchIn& in)
{
    WatchOut out;
    bool     cut = false;
    {
        std::string low = in.top;
        for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        cut = low.compare(0, 3, "cut") == 0 || low.find(" cut") != std::string::npos || low.find("couper") != std::string::npos;
        for (const std::string& l : in.cut_names) cut = cut || (!l.empty() && in.top.find(l) != std::string::npos);
    }
    const bool user = in.new_undo && !in.redo && !in.top.empty() && in.top.compare(0, 3, "RAV") != 0 && !cut;
    if (user && in.scope != Scope::Item) {
        if (IsDeleteUndoName(in.top, in.delete_names)) {
            for (const Instance& i : last.inst)
                if (!now.present.count(i.guid) && now.owners.count(i.owner) && last.selected.count(i.guid)) out.gone.insert(i.guid);
            return out;  // a delete: nothing else is read from it
        }
        out.looks = PlanPropagatedLooks(last.inst, last.looks, now.looks, in.scope);
        // A drag: only after a click, never when an animation item moved (RAV's follow moved its sounds).
        bool owner_moved = false;
        for (const auto& kv : now.owners) {
            const auto b = last.owners.find(kv.first);
            owner_moved = owner_moved || (b != last.owners.end() && std::fabs(b->second - kv.second) > 1e-9);
        }
        if (in.clicked && !owner_moved && !in.expanded.empty()) {
            const std::string dragged = in.expanded.count(in.under_mouse) ? in.under_mouse : in.anchor;
            out.back = PlanMoveBack(in.expanded, dragged, last.pos, now.pos);
        }
    }
    if (!out.looks.empty() || !out.back.empty()) return out;
    // A click: one seen (down then up), or one too quick for the timer to see: the selection changed to exactly one item,
    // the one under the mouse (a script's selection is not under it; RAV's own selections are never read as a change).
    const bool click = in.clicked || (now.selected.size() == 1 && !in.under_mouse.empty() && now.selected.count(in.under_mouse));
    if (click && now.selected != last.selected) {
        out.select = ExpandSelection(now.inst, now.selected, in.scope, in.ctrl);
        out.clear_expanded = out.select.empty() && now.selected != in.expanded;
    }
    return out;
}

}  // namespace am
}  // namespace rav
