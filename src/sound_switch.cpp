// SPDX-License-Identifier: MIT
//
// See sound_switch.h. Story 13-4: pure functions over sound_model.h; Random picks, variations and
// missing media come only from ResolveHooks. Every walk keeps the ancestors it is inside, so a
// malformed store (a cycle) never loops.

#include "sound_switch.h"

#include <algorithm>
#include <set>

#include "sound_record.h"

namespace rav {

namespace {

const char* const kSep = " \xE2\x80\xBA ";    // " › "
const char* const kArrow = " \xE2\x86\x92 ";  // " → "
const char* const kTimes = "\xC3\x97";        // "×"

bool Contains(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

// The child a Switch case plays for `value`, nullptr when it has no content (D2, D3).
const SoundNode* CaseChild(const SoundLibrary& lib, const SoundNode& sw, const std::string& value)
{
    if (value.empty()) return nullptr;
    for (const SwitchCase& c : sw.cases) {
        if (c.value_uid != value) continue;
        if (c.child_uid.empty() || Contains(sw.unassigned, c.child_uid)) return nullptr;
        const SoundNode* n = FindNode(lib, c.child_uid);
        if (!n || n->parent != sw.uid || n->uid == sw.uid) return nullptr;
        return n;
    }
    return nullptr;
}

// A value's name for the path text; a uid no longer in the group (a deleted value still set on a
// layer) reads "(deleted value)", never the raw uid.
std::string ValueName(const SwitchGroup* g, const std::string& value)
{
    if (g)
        for (const SwitchValue& v : g->values)
            if (v.uid == value) return v.name;
    return "(deleted value)";
}

std::string GroupLabel(const SwitchGroup* g, const SoundNode& sw)
{
    return g ? g->name : sw.name;
}

struct Walker {
    const SoundStore&   store;
    const SoundLibrary& lib;
    const ValueLayers&  layers;
    const ResolveHooks& hooks;
    Resolution&         res;
    std::set<std::string> stack;
    bool                text_open = true;  // the path text still follows this branch

    void Seg(const std::string& s)
    {
        if (!text_open) return;
        if (!res.path_text.empty()) res.path_text += kSep;
        res.path_text += s;
    }

    void Walk(const SoundNode& n, double gain, double offset)
    {
        if (stack.count(n.uid)) return;
        stack.insert(n.uid);
        res.path.push_back(n.uid);
        gain += n.volume_db;
        if (!n.parent.empty())
            if (const SoundNode* p = FindNode(lib, n.parent))
                if (p->type == SoundNodeType::Blend) offset += n.offset_ms;

        switch (n.type) {
        case SoundNodeType::Sound: {
            ResolvedSound rs;
            rs.node = n.uid;
            rs.offset_ms = offset;
            rs.gain_db = gain;
            if (hooks.is_missing && hooks.is_missing(n)) {
                rs.missing = true;
                rs.variation = -1;
            } else {
                rs.variation = hooks.variation ? hooks.variation(n) : -1;
                if (rs.variation >= 0 && rs.variation < (int)n.sound.frames.size())
                    rs.gain_db += n.sound.frames[(size_t)rs.variation].db;
            }
            res.sounds.push_back(rs);
            break;
        }
        case SoundNodeType::Random: {
            std::vector<const SoundNode*> kids = ChildrenOf(lib, n.uid);
            int idx = hooks.random_child ? hooks.random_child(n) : 0;
            if (idx < 0 || idx >= (int)kids.size()) {
                Seg("Random");
                text_open = false;
                break;
            }
            const SoundNode* c = kids[(size_t)idx];
            Seg("Random " + c->name);
            if (c->type == SoundNodeType::Sound) text_open = false;
            Walk(*c, gain, offset);
            break;
        }
        case SoundNodeType::Blend: {
            std::vector<const SoundNode*> kids = ChildrenOf(lib, n.uid);
            Seg(std::string("Blend ") + kTimes + std::to_string(kids.size()));
            text_open = false;
            for (const SoundNode* c : kids) Walk(*c, gain, offset);
            break;
        }
        case SoundNodeType::Switch: {
            const SwitchGroup* g = FindGroup(store, n.group_uid);
            ValueFrom from = ValueFrom::Default;
            std::string v = EffectiveValue(n, layers, &from);
            std::string def = DefaultValue(store, n);
            const SoundNode* c = nullptr;
            std::string label = GroupLabel(g, n) + " ";
            if (from == ValueFrom::Default) {
                c = CaseChild(lib, n, def);
                label += ValueName(g, def) + " (default)";
            } else if ((c = CaseChild(lib, n, v))) {
                label += ValueName(g, v);
            } else {
                c = CaseChild(lib, n, def);
                label += ValueName(g, v) + kArrow + ValueName(g, def) + " (default)";
            }
            Seg(label);
            if (c) {
                if (c->type == SoundNodeType::Sound) {
                    Seg(c->name);
                    text_open = false;
                }
                Walk(*c, gain, offset);
            } else {
                text_open = false;
            }
            break;
        }
        }
        stack.erase(n.uid);
    }
};

struct Checker {
    const SoundStore*   store;  // nullptr = the 1-argument form (no group checks)
    const SoundLibrary& lib;
    LibraryCheck&       out;
    std::set<std::string> stack;

    void Block(const SoundNode& n, const std::string& why)
    {
        if (!out.blocked) out.reason = why.empty() ? n.name : n.name + ": " + why;
        out.blocked = true;
        if (!Contains(out.nodes, n.uid)) out.nodes.push_back(n.uid);
    }

    void Walk(const SoundNode& n)
    {
        if (stack.count(n.uid)) {
            if (!out.blocked) out.reason = n.name + ": cycle";
            out.blocked = true;
            if (!Contains(out.nodes, n.uid)) out.nodes.push_back(n.uid);
            return;
        }
        stack.insert(n.uid);
        switch (n.type) {
        case SoundNodeType::Sound:
            break;
        case SoundNodeType::Random:
        case SoundNodeType::Blend: {
            std::vector<const SoundNode*> kids = ChildrenOf(lib, n.uid);
            if (kids.empty()) Block(n, n.type == SoundNodeType::Random ? "empty Random" : "empty Blend");
            for (const SoundNode* c : kids) Walk(*c);
            break;
        }
        case SoundNodeType::Switch: {
            static const SoundStore kEmpty;
            const SoundStore& st = store ? *store : kEmpty;
            if (store) {
                const SwitchGroup* g = FindGroup(st, n.group_uid);
                if (!g) Block(n, "Switch Group not found");
                else if (g->values.empty()) Block(n, "Switch Group has no value");
            }
            if (!CaseChild(lib, n, DefaultValue(st, n))) Block(n, "default value has no sound");
            std::set<std::string> seen;
            for (const SwitchCase& c : n.cases) {
                const SoundNode* k = CaseChild(lib, n, c.value_uid);
                if (k && seen.insert(k->uid).second) Walk(*k);
            }
            break;
        }
        }
        stack.erase(n.uid);
    }
};

// Both CheckLibrary forms; store nullptr skips the Switch Group checks.
LibraryCheck Check(const SoundStore* store, const SoundLibrary& lib)
{
    LibraryCheck out;
    const SoundNode* root = lib.root.empty() ? nullptr : FindNode(lib, lib.root);
    if (!root) {
        out.blocked = true;
        out.reason = "no root";
        return out;
    }
    Checker c{store, lib, out, {}};
    c.Walk(*root);
    return out;
}

}  // namespace

std::string EffectiveValue(const SoundNode& sw, const ValueLayers& layers, ValueFrom* from)
{
    auto find = [&](const std::vector<ValueSet>& vs) -> const ValueSet* {
        for (const ValueSet& v : vs)
            if (v.group_uid == sw.group_uid && !v.value_uid.empty()) return &v;
        return nullptr;
    };
    if (const ValueSet* v = find(layers.event)) {
        if (from) *from = ValueFrom::Event;
        return v->value_uid;
    }
    if (const ValueSet* v = find(layers.rule)) {
        if (from) *from = ValueFrom::Rule;
        return v->value_uid;
    }
    if (const ValueSet* v = find(layers.item)) {
        if (from) *from = ValueFrom::Item;
        return v->value_uid;
    }
    if (from) *from = ValueFrom::Default;
    return sw.default_value;
}

std::string DefaultValue(const SoundStore& store, const SoundNode& sw)
{
    if (!sw.default_value.empty()) return sw.default_value;
    if (const SwitchGroup* g = FindGroup(store, sw.group_uid))
        return g->values.empty() ? std::string() : g->values.front().uid;
    return sw.cases.empty() ? std::string() : sw.cases.front().value_uid;
}

LibraryCheck CheckLibrary(const SoundStore& store, const SoundLibrary& lib)
{
    return Check(&store, lib);
}

LibraryCheck CheckLibrary(const SoundLibrary& lib)
{
    return Check(nullptr, lib);
}

Resolution ResolveEvent(const SoundStore& store, const SoundLibrary& lib, const ValueLayers& layers,
                        const ResolveHooks& hooks)
{
    Resolution res;
    LibraryCheck chk = CheckLibrary(store, lib);
    if (chk.blocked) {
        res.blocked = true;
        res.reason = chk.reason;
        return res;
    }
    const SoundNode* root = FindNode(lib, lib.root);
    Walker w{store, lib, layers, hooks, res, {}, true};
    if (root->type == SoundNodeType::Sound) {
        w.Seg(root->name);
        w.text_open = false;
    }
    w.Walk(*root, 0.0, 0.0);
    return res;
}

bool ConvertNodeType(SoundLibrary& lib, const std::string& uid, SoundNodeType type, const std::string& group_uid,
                     const SoundStore& store)
{
    if (type == SoundNodeType::Sound) return false;
    SoundNode* n = FindNode(lib, uid);
    if (!n || n->type == SoundNodeType::Sound) return false;

    if (type != SoundNodeType::Switch) {
        if (n->type == type) return false;
        if (n->type == SoundNodeType::Switch) {
            n->group_uid.clear();
            n->cases.clear();
            n->default_value.clear();
            n->preview_value.clear();
            n->unassigned.clear();
        }
        n->type = type;
        return true;
    }

    const SwitchGroup* g = FindGroup(store, group_uid);
    if (!g) return false;
    if (n->type == SoundNodeType::Switch && n->group_uid == group_uid) return false;

    if (n->type != SoundNodeType::Switch) {
        std::vector<const SoundNode*> kids = ChildrenOf(lib, uid);
        std::vector<std::string> kid_uids, kid_names;
        for (const SoundNode* k : kids) {
            kid_uids.push_back(k->uid);
            kid_names.push_back(k->name);
        }
        n->cases.clear();
        n->unassigned.clear();
        std::vector<bool> taken(g->values.size(), false);
        for (size_t i = 0; i < kid_uids.size(); ++i) {
            std::string key = SwitchNameKey(kid_names[i]);
            bool placed = false;
            for (size_t j = 0; j < g->values.size(); ++j) {
                if (taken[j] || SwitchNameKey(g->values[j].name) != key) continue;
                taken[j] = true;
                SwitchCase c;
                c.value_uid = g->values[j].uid;
                c.child_uid = kid_uids[i];
                n->cases.push_back(c);
                placed = true;
                break;
            }
            if (!placed) n->unassigned.push_back(kid_uids[i]);
        }
        // Decision 2026-10-12: the default is the first group value (in group order) that
        // received a child, else none.
        n->default_value.clear();
        for (size_t j = 0; j < g->values.size(); ++j) {
            if (!taken[j]) continue;
            n->default_value = g->values[j].uid;
            break;
        }
        n->preview_value.clear();
        n->group_uid = group_uid;
        n->type = SoundNodeType::Switch;
        return true;
    }

    // Switch -> Switch on another group: map by value name.
    const SwitchGroup* old_g = FindGroup(store, n->group_uid);
    auto map_value = [&](const std::string& old_uid) -> std::string {
        if (!old_g) return std::string();
        for (const SwitchValue& ov : old_g->values) {
            if (ov.uid != old_uid) continue;
            std::string key = SwitchNameKey(ov.name);
            for (const SwitchValue& v : g->values)
                if (SwitchNameKey(v.name) == key) return v.uid;
            break;
        }
        return std::string();
    };
    std::vector<SwitchCase> cases;
    std::vector<std::string> unassigned = n->unassigned;
    std::vector<std::string> dropped;  // children of unmatched cases
    for (const SwitchCase& c : n->cases) {
        std::string nv = map_value(c.value_uid);
        bool dup = false;
        for (const SwitchCase& e : cases)
            if (e.value_uid == nv) dup = true;
        if (!nv.empty() && !dup) {
            SwitchCase m = c;
            m.value_uid = nv;
            cases.push_back(m);
        } else if (!c.child_uid.empty()) {
            dropped.push_back(c.child_uid);
        }
    }
    // A dropped child joins Unassigned unless another case still plays it.
    for (const std::string& k : dropped) {
        bool still_cased = false;
        for (const SwitchCase& e : cases)
            if (e.child_uid == k) still_cased = true;
        if (!still_cased && !Contains(unassigned, k)) unassigned.push_back(k);
    }
    // The default maps by name, including an implicit one (the old group's first value).
    std::string old_def = DefaultValue(store, *n);
    n->default_value = old_def.empty() ? std::string() : map_value(old_def);
    n->preview_value = n->preview_value.empty() ? std::string() : map_value(n->preview_value);
    n->cases = cases;
    n->unassigned = unassigned;
    n->group_uid = group_uid;
    return true;
}

std::string SwitchNameKey(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        if (ch == '_' || ch == '-' || ch == ' ') continue;
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        out.push_back(ch);
    }
    return out;
}

}  // namespace rav
