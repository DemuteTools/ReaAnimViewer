// SPDX-License-Identifier: MIT
//
// See attach_manager_plan.h (13-68e).

#include "attach_manager_plan.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "sound_record.h"        // FindNode, ChildrenOf, FindGroup
#include "sound_spread_plan.h"  // RemoveSubtree

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

bool Live(Ownership o)
{
    return o != Ownership::Deleted;
}

// The node a placement plays: the record's, else the library Sound whose source it is.
std::string NodeOf(const SoundPlacement& p, const std::map<std::string, std::string>& node_of)
{
    if (!p.node.empty()) return p.node;
    const auto hit = node_of.find(GuidKey(p.item_guid));
    return hit == node_of.end() ? std::string() : hit->second;
}

// The event's draw of `random` (any pass) is `index`.
bool DrawsIndex(const std::vector<SoundDraw>& draws, const std::string& random, int index)
{
    if (random.empty() || index < 0) return false;
    for (const SoundDraw& d : draws)
        if (DrawNode(d.node) == random && d.pick == index) return true;
    return false;
}

}  // namespace

std::string GuidKey(const std::string& g)
{
    std::string out = g;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// ---- Attach tab ------------------------------------------------------------------------------------------------

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

GestureChoice ChooseGesture(const Snapshot& s, const std::vector<int>& choices, int marker, GestureWant want)
{
    GestureChoice c;
    std::set<int>         evs;
    std::set<std::string> keys;
    for (const StackRef& r : s.stack_refs) {
        evs.insert(r.event);
        keys.insert(GuidKey(r.owner) + "|" + r.rid + "|" + std::to_string(static_cast<long long>(std::llround(r.t * 1000.0))));
    }
    c.stack_events = static_cast<int>(std::max(evs.size(), keys.size()));
    if (c.stack_events == 1) c.stack_event = *evs.begin();
    c.stack_ok = !s.stack_refs.empty() && s.fresh > 0;
    const bool auto_stack = c.stack_ok;
    c.gesture = want == GestureWant::Stack ? Gesture::Stack : want == GestureWant::Attach ? Gesture::Attach
                                                            : (auto_stack ? Gesture::Stack : Gesture::Attach);
    if (c.gesture == Gesture::Stack) {
        if (!c.stack_ok) {
            c.reason = "Select an attached sound with the new items.";
            c.from = "Add to stack: no attached sound with new items in the selection.";
        } else if (c.stack_events > 1) {
            c.reason = "Select attached sounds of one marker only (here " + std::to_string(c.stack_events) + " markers).";
            c.from = "The selection holds sounds of " + std::to_string(c.stack_events) + " different markers.";
        } else {
            c.from = c.stack_event >= 0 ? "From the selected attached sound." : "From the selected attached sound (its marker is not read now).";
        }
        return c;
    }
    c.marker = marker;
    std::vector<std::string> seen;
    bool                     clicked = false;
    for (int i : choices) {
        const Marker& m = s.markers[static_cast<size_t>(i)];
        const bool    in_range = s.has_time_sel && m.pos >= s.ts0 - 1e-9 && m.pos <= s.ts1 + 1e-9;
        if (in_range) seen.push_back(m.label.empty() ? m.name : m.label);
        clicked |= m.clicked;
    }
    std::string from;
    if (!seen.empty()) {
        from = "Time selection: ";
        for (size_t i = 0; i < seen.size(); ++i) from += (i ? ", " : "") + seen[i];
        from += ".";
    }
    if (clicked)
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
    if (s.sounds <= 0) c.reason = "Select the sounds to attach.";
    return c;
}

std::string RunLabel(Gesture g, bool all, bool side_only, const std::string& kind, char side, bool other_none)
{
    std::string out = g == Gesture::Stack ? "Add to stack" : "Attach";
    out += kDot;
    out += all ? "all with " + kind : std::string("this animation");
    out += kDot;
    out += side_only && side ? std::string(1, side) + " only" : std::string("both sides");
    if (side_only && side)  // 13-68e v4: what the other side becomes
        out += kDot + std::string(1, side == 'L' ? 'R' : 'L') + (other_none ? " plays nothing" : " keeps its sound");
    return out;
}

// ---- Detach tab: what the list shows -----------------------------------------------------------------------

ListFilter MakeFilter(const Snapshot& s)
{
    ListFilter f;
    std::vector<const Anim*> sa;
    for (const Anim& a : s.anims)
        if (a.selected) sa.push_back(&a);
    if (!sa.empty()) {
        f.kind = FilterKind::Animations;
        for (const Anim* a : sa) f.anims.insert(GuidKey(a->guid));
        f.label = sa.size() > 1 ? std::to_string(sa.size()) + " selected animations" : "the selected animation (" + sa.front()->name + ")";
        return f;
    }
    if (!s.selected_items.empty()) {
        f.kind = FilterKind::Items;
        f.items = s.selected_items;
        int         placed = 0;
        std::string one;
        for (const AttachedEvent& e : s.events)
            for (const Placed& p : e.placed)
                if (f.items.count(GuidKey(p.item_guid))) {
                    ++placed;
                    one = p.name;
                }
        const size_t n = s.selected_items.size();
        if (placed == 0) f.label = n > 1 ? "the selected items (not attached)" : "the selected item (not attached)";
        else if (placed == 1 && n == 1) f.label = "the selected item (" + one + ")";
        else f.label = std::to_string(n) + " selected items";
        return f;
    }
    if (s.has_time_sel) {
        f.kind = FilterKind::TimeSelection;
        f.t0 = s.ts0;
        f.t1 = s.ts1;
        char b[96];
        std::snprintf(b, sizeof(b), "the time selection (%.2f \xE2\x80\x93 %.2f s)", s.ts0, s.ts1);
        f.label = b;
        return f;
    }
    f.label = "everything";
    return f;
}

bool FilterKeeps(const Snapshot& s, const ListFilter& f, const AttachedEvent& e)
{
    switch (f.kind) {
    case FilterKind::Animations:
        return e.anim >= 0 && static_cast<size_t>(e.anim) < s.anims.size() && f.anims.count(GuidKey(s.anims[static_cast<size_t>(e.anim)].guid));
    case FilterKind::Items:
        for (const Placed& p : e.placed)
            if (f.items.count(GuidKey(p.item_guid))) return true;
        return false;
    case FilterKind::TimeSelection: return e.pos >= f.t0 - 1e-9 && e.pos <= f.t1 + 1e-9;
    case FilterKind::Everything: return true;
    }
    return true;
}

std::vector<AnimGroup> MarkerRows(const Snapshot& s, const ListFilter& f, const std::string& lib_uid, char side)
{
    std::vector<int> evs;
    for (size_t i = 0; i < s.events.size(); ++i) {
        const AttachedEvent& e = s.events[i];
        if (!lib_uid.empty() && e.lib_uid != lib_uid) continue;
        if (side && e.side != side) continue;  // 13-68e v4
        if (e.placed.empty() || !FilterKeeps(s, f, e)) continue;
        evs.push_back(static_cast<int>(i));
    }
    std::stable_sort(evs.begin(), evs.end(), [&](int a, int b) { return s.events[static_cast<size_t>(a)].pos < s.events[static_cast<size_t>(b)].pos; });
    std::vector<AnimGroup> out;
    for (int i : evs) {
        const AttachedEvent& e = s.events[static_cast<size_t>(i)];
        AnimGroup*           g = nullptr;
        for (AnimGroup& x : out)
            if (x.anim == e.anim) g = &x;
        if (!g) {
            AnimGroup ng;
            ng.anim = e.anim;
            ng.name = e.anim >= 0 && static_cast<size_t>(e.anim) < s.anims.size() ? s.anims[static_cast<size_t>(e.anim)].name : std::string();
            out.push_back(ng);
            g = &out.back();
        }
        MarkerRow r;
        r.event = i;
        // The base: the first sound on the event itself (offset 0), else the first; the others are its layers.
        const Placed* base = nullptr;
        bool          original = false;
        for (const Placed& p : e.placed) {
            if (!base && std::fabs(p.offset_s) < 1e-9) base = &p;
            original |= p.own == Ownership::Attached && std::fabs(p.offset_s) < 1e-9;
        }
        if (!base) base = &e.placed.front();
        std::set<std::string> names;
        int                   layers = 0;
        for (const Placed& p : e.placed)
            if (&p != base && names.insert(p.name + "|" + p.item_guid).second) ++layers;
        r.l1 = (base->name.empty() ? std::string("Sound") : base->name) + (layers ? " + " + Plural(layers, "layer", "layers") : std::string());
        r.l2 = (e.label.empty() ? std::string("Marker") : e.label) + kDot + Seconds(e.pos) + kDot + (original ? "attached original" : "copy");
        g->rows.push_back(r);
    }
    return out;
}

// ---- Detach tab: Whole library -----------------------------------------------------------------------------

std::set<std::string> SoundsUnder(const SoundLibrary& lib, const std::string& uid)
{
    std::set<std::string> in, out;
    if (!FindNode(lib, uid)) return out;
    in.insert(uid);
    for (bool grew = true; grew;) {
        grew = false;
        for (const SoundNode& n : lib.nodes)
            if (!in.count(n.uid) && in.count(n.parent)) {
                in.insert(n.uid);
                grew = true;
            }
    }
    for (const SoundNode& n : lib.nodes)
        if (in.count(n.uid) && n.type == SoundNodeType::Sound) out.insert(n.uid);
    return out;
}

bool EventUsesVariation(const SoundLibrary& lib, const VariationRow& v, const AttachedEvent& e)
{
    // As EventUsedRemoved (what Remove from library releases): a sound from a Sound under it, or its Random's draw.
    if (e.lib_uid != lib.uid || e.placed.empty()) return false;
    const std::set<std::string> sounds = SoundsUnder(lib, v.node);
    for (const Placed& p : e.placed)
        if (!p.node.empty() && sounds.count(p.node)) return true;
    return DrawsIndex(e.draws, v.random, v.index);
}

namespace {

struct RowWalk {
    const SoundStore&          store;
    const SoundLibrary&        lib;
    std::vector<VariationRow>* out;
    int                        guard = 0;
    char                       side = 0;  // the Side Switch case walked under (0 = none)

    void Row(const SoundNode& n, const std::string& where, bool layer, const std::string& random, int index)
    {
        VariationRow r;
        r.lib_uid = lib.uid;
        r.lib_name = lib.name;
        r.node = n.uid;
        r.name = n.name.empty() ? std::string("Sound") : n.name;
        r.where = where;
        r.random = random;
        r.index = index;
        r.side = side;
        if (layer) r.kind = VariationKind::Layer;
        else if (n.type == SoundNodeType::Sound && n.sound.mode == DetectMode::Takes && n.sound.frames.size() >= 2) {
            r.kind = VariationKind::Takes;
            r.takes = static_cast<int>(n.sound.frames.size());
        }
        out->push_back(r);
    }

    static std::string Join(const std::string& a, const std::string& b)
    {
        return a.empty() ? b : b.empty() ? a : a + kDot + b;
    }

    // `layers`: every child of a Blend here is a layer (a per-animation / per-side layers Blend).
    void Walk(const SoundNode& n, const std::string& where, bool layers)
    {
        if (++guard > static_cast<int>(lib.nodes.size()) + 4) return;  // a damaged store's loop
        const std::vector<const SoundNode*> kids = ChildrenOf(lib, n.uid);
        switch (n.type) {
        case SoundNodeType::Sound: Row(n, where, layers, std::string(), -1); break;
        case SoundNodeType::Random:
            if (layers) {
                Row(n, where, true, std::string(), -1);
                break;
            }
            for (size_t k = 0; k < kids.size(); ++k) Row(*kids[k], where, false, n.uid, static_cast<int>(k));
            break;
        case SoundNodeType::Blend:
            for (size_t k = 0; k < kids.size(); ++k) {
                const SoundNode& c = *kids[k];
                if (k == 0 && !layers) Walk(c, where, false);  // the base
                else if (c.type == SoundNodeType::Switch) Walk(c, where, true);
                else Row(c, where, true, std::string(), -1);
            }
            break;
        case SoundNodeType::Switch: {
            const SwitchGroup* g = FindGroup(store, n.group_uid);
            const bool         side_sw = g && IsSideGroup(*g);
            for (const SwitchCase& c : n.cases) {
                const SoundNode* child = FindNode(lib, c.child_uid);
                if (!child) continue;
                std::string vname;
                if (g)
                    for (const SwitchValue& v : g->values)
                        if (v.uid == c.value_uid) vname = v.name;
                const char        was = side;
                if (side_sw)  // 13-68e v4: said apart, as where it plays
                    side = c.value_uid == SideValueOf(*g, 'L') ? 'L' : c.value_uid == SideValueOf(*g, 'R') ? 'R' : 0;
                const std::string w = side_sw ? where : Join(where, vname);
                if (!layers) Walk(*child, w, false);  // a Switch at the content's top: each value's content
                else if (child->type == SoundNodeType::Blend) Walk(*child, w, true);
                else if (child->type == SoundNodeType::Switch) Walk(*child, w, true);
                else Row(*child, w, true, std::string(), -1);
                side = was;
            }
            break;
        }
        }
    }
};

}  // namespace

std::vector<VariationRow> VariationRows(const SoundStore& store, const SoundLibrary& lib, const Snapshot& s, char side)
{
    std::vector<VariationRow> out;
    const SoundNode*          root = lib.root.empty() ? nullptr : FindNode(lib, lib.root);
    if (!root) return out;
    RowWalk w{store, lib, &out};
    w.Walk(*root, std::string(), false);
    const bool sided = LibrarySided(store, lib, s);
    std::vector<VariationRow> kept;
    for (VariationRow& r : out) {
        r.sided = sided;
        if (side && r.side && r.side != side) continue;  // 13-68e v4: not played on that side
        for (const AttachedEvent& e : s.events)
            if (!side || e.side == side) r.markers += EventUsesVariation(lib, r, e) ? 1 : 0;
        kept.push_back(r);
    }
    return kept;
}

std::vector<std::string> VariationItems(const SoundLibrary& lib, const VariationRow& v, const Snapshot& s, char side)
{
    std::vector<std::string>    out;
    std::set<std::string>       seen;
    const std::set<std::string> sounds = SoundsUnder(lib, v.node);
    for (const AttachedEvent& e : s.events) {
        if (side && e.side != side) continue;  // 13-68e v4
        if (!EventUsesVariation(lib, v, e)) continue;
        for (const Placed& p : e.placed) {
            const bool mine = (!p.node.empty() && sounds.count(p.node)) ||
                              (p.node.empty() && !v.random.empty() && std::fabs(p.offset_s) < 1e-9);
            if (mine && seen.insert(GuidKey(p.item_guid)).second) out.push_back(p.item_guid);
        }
    }
    return out;
}

std::string VariationText(const VariationRow& v)
{
    std::string out = v.kind == VariationKind::Layer ? "layer" : v.kind == VariationKind::Takes ? std::to_string(v.takes) + " takes" : "variation";
    if (v.sided) out += kDot + (v.side ? std::string(1, v.side) + " only" : std::string("L \xC2\xB7 R"));  // 13-68e v4
    if (!v.where.empty()) out += kDot + v.where;
    out += kDot + std::string("on ") + Plural(v.markers, "marker", "markers");
    return out;
}

// ---- The record edits ----------------------------------------------------------------------------------------

std::vector<std::string> DetachEventInRecord(EventSounds& ev, const SoundLibrary& lib)
{
    std::vector<std::string> out;
    std::set<std::string>    seen, marked;
    std::string              any_guid;
    if (std::none_of(ev.placed.begin(), ev.placed.end(), [](const SoundPlacement& p) { return Live(p.own); }))
        return out;  // nothing live here (already detached, a stale list): nothing changes
    for (SoundPlacement& p : ev.placed) {
        if (!Live(p.own)) {
            marked.insert(p.node);
            continue;
        }
        if (seen.insert(GuidKey(p.item_guid)).second) out.push_back(p.item_guid);
        if (any_guid.empty()) any_guid = p.item_guid;
        p.own = Ownership::Deleted;
        if (!p.node.empty()) marked.insert(p.node);
    }
    // Every Sound of the library marked deleted on this event: Populate never places one here again.
    for (const SoundNode& n : lib.nodes) {
        if (n.type != SoundNodeType::Sound || marked.count(n.uid)) continue;
        SoundPlacement p;
        p.item_guid = any_guid;
        p.node = n.uid;
        p.own = Ownership::Deleted;
        ev.placed.push_back(p);
        marked.insert(n.uid);
    }
    return out;
}

bool RemoveVariationFromLibrary(SoundLibrary& lib, const std::string& node, RemovedVariation* out)
{
    const SoundNode* n = FindNode(lib, node);
    if (!n || !out) return false;
    RemovedVariation r;
    r.node = node;
    if (const SoundNode* p = FindNode(lib, n->parent); p && p->type == SoundNodeType::Random) {
        r.random = p->uid;
        const std::vector<const SoundNode*> kids = ChildrenOf(lib, p->uid);
        for (size_t k = 0; k < kids.size(); ++k)
            if (kids[k]->uid == node) r.index = static_cast<int>(k);
        r.siblings = SoundsUnder(lib, p->uid);
    }
    r.sounds = SoundsUnder(lib, node);
    if (r.siblings.empty()) r.siblings = r.sounds;
    const std::vector<std::string> gone = RemoveSubtree(lib, node);
    r.removed.insert(gone.begin(), gone.end());
    *out = r;
    return !gone.empty();
}

bool EventUsedRemoved(const EventSounds& ev, const RemovedVariation& r, const std::map<std::string, std::string>& node_of)
{
    bool live = false;
    for (const SoundPlacement& p : ev.placed) {
        if (!Live(p.own)) continue;
        live = true;
        const std::string node = NodeOf(p, node_of);
        if (!node.empty() && r.sounds.count(node)) return true;
    }
    return live && DrawsIndex(ev.draws, r.random, r.index);
}

std::vector<std::string> RemoveVariationInRecord(ItemSounds& rec, const std::set<std::string>& rids, const RemovedVariation& r,
                                                 bool keep, const std::map<std::string, std::string>& node_of, int* events)
{
    std::vector<std::string> out;
    std::set<std::string>    seen;
    int                      n_events = 0;
    for (EventSounds& ev : rec.events) {
        if (!rids.count(ev.rid)) continue;
        const bool used = EventUsedRemoved(ev, r, node_of);
        if (used) {
            ++n_events;
            const bool drawn = DrawsIndex(ev.draws, r.random, r.index);
            // Its items: placed from a Sound under it, or (an attached original whose Sound is not known) the event's
            // own sound when its Random drew it.
            std::vector<size_t> mine;
            for (size_t i = 0; i < ev.placed.size(); ++i) {
                const SoundPlacement& p = ev.placed[i];
                if (!Live(p.own)) continue;
                const std::string node = NodeOf(p, node_of);
                if ((!node.empty() && r.sounds.count(node)) ||
                    (node.empty() && drawn && p.own != Ownership::Populated && std::fabs(p.offset_s) < 1e-9))
                    mine.push_back(i);
            }
            for (size_t i : mine)
                if (seen.insert(GuidKey(ev.placed[i].item_guid)).second) out.push_back(ev.placed[i].item_guid);
            if (keep) {
                std::set<std::string> marked;
                std::string           guid;
                for (size_t i : mine) {
                    SoundPlacement& p = ev.placed[i];
                    if (guid.empty()) guid = p.item_guid;
                    p.own = Ownership::Deleted;
                    if (p.node.empty()) p.node = NodeOf(p, node_of);
                }
                for (const SoundPlacement& p : ev.placed)
                    if (!Live(p.own)) marked.insert(p.node);
                for (const std::string& s : r.siblings) {  // what its Random could draw instead: not placed here
                    if (marked.count(s) || r.sounds.count(s)) continue;
                    SoundPlacement p;
                    p.item_guid = guid;
                    p.node = s;
                    p.own = Ownership::Deleted;
                    ev.placed.push_back(p);
                    marked.insert(s);
                }
            } else {
                std::vector<SoundPlacement> left;
                for (size_t i = 0; i < ev.placed.size(); ++i)
                    if (std::find(mine.begin(), mine.end(), i) == mine.end()) left.push_back(ev.placed[i]);
                ev.placed = std::move(left);
            }
            ev.draws.erase(std::remove_if(ev.draws.begin(), ev.draws.end(),
                                          [&](const SoundDraw& d) {
                                              const std::string node = DrawNode(d.node);
                                              return node == r.random || r.removed.count(node) > 0;
                                          }),
                           ev.draws.end());
        }
        // Every other draw of the Random keeps its child: the ones after the removed index move down.
        if (!r.random.empty() && r.index >= 0)
            for (SoundDraw& d : ev.draws)
                if (DrawNode(d.node) == r.random && d.pick > r.index) --d.pick;
    }
    if (events) *events = n_events;
    return out;
}

bool LibrarySided(const SoundStore& store, const SoundLibrary& lib, const Snapshot& s)
{
    if (LibraryUsesSide(store, lib)) return true;
    for (const AttachedEvent& e : s.events)
        if (e.lib_uid == lib.uid && e.side != 0) return true;
    return false;
}

void SplitRidsBySide(const std::set<std::string>& rids, const std::map<std::string, char>& side_of, char side,
                     std::set<std::string>* mine, std::set<std::string>* other)
{
    for (const std::string& rid : rids) {
        if (!side) {
            mine->insert(rid);
            continue;
        }
        const auto  hit = side_of.find(rid);
        const char  sd = hit == side_of.end() ? 0 : hit->second;
        if (sd == side) mine->insert(rid);
        else if (sd == 'L' || sd == 'R') other->insert(rid);  // a sideless or unread rule: neither
    }
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

}  // namespace am
}  // namespace rav
