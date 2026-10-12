// SPDX-License-Identifier: MIT
//
// See assign_view_model.h.

#include "assign_view_model.h"

#include <algorithm>
#include <map>
#include <set>

#include "sound_auto_assign.h"
#include "sound_populate_plan.h"  // GroupsUsed
#include "sound_record.h"         // FindLibrary, FindGroup, FindRuleSounds

namespace rav {
namespace assign {
namespace {

const char kDot[] = " \xC2\xB7 ";            // " · "
const char kArrow[] = " \xE2\x80\xBA ";      // " › "

std::string Lower(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

std::string ValueNameOf(const SwitchGroup* g, const std::string& uid)
{
    if (g)
        for (const SwitchValue& v : g->values)
            if (v.uid == uid) return v.name;
    return uid.empty() ? std::string("none") : std::string("?");
}

const ValueSet* FindSet(const std::vector<ValueSet>& vals, const std::string& group)
{
    for (const ValueSet& v : vals)
        if (v.group_uid == group && !v.value_uid.empty()) return &v;
    return nullptr;
}

bool HasSound(const SoundLibrary& lib)
{
    for (const SoundNode& n : lib.nodes)
        if (n.type == SoundNodeType::Sound) return true;
    return false;
}

// Why a library is blocked, in the window's words.
std::string BlockedText(const SoundStore& store, const SoundLibrary& lib, const LibraryCheck& chk)
{
    for (const GroupUse& g : GroupsUsed(store, lib))
        if (!FindGroup(store, g.group_uid)) return "a Switch lost its Switch Group (deleted in the Library view)";
    return chk.reason.empty() ? std::string("it cannot place a sound") : chk.reason;
}

// The text of the lowest ancestor of `sw` the walk reached, when it went another way: "not reached under Crouch".
std::string NotReachedText(const SoundStore& store, const SoundLibrary& lib, const std::set<std::string>& path,
                           const SoundNode& sw)
{
    const SoundNode* cur = &sw;
    while (cur && !cur->parent.empty()) {
        const SoundNode* p = FindNode(lib, cur->parent);
        if (!p) break;
        if (path.count(p->uid)) {
            if (p->type == SoundNodeType::Switch) {
                for (const SoundNode* kid : ChildrenOf(lib, p->uid)) {
                    if (!path.count(kid->uid)) continue;
                    for (const SwitchCase& c : p->cases)
                        if (c.child_uid == kid->uid)
                            return "not reached under " + ValueNameOf(FindGroup(store, p->group_uid), c.value_uid);
                }
            }
            break;
        }
        cur = p;
    }
    return "not reached";
}

std::string SourceLabel(const std::string& owner, const std::string& part, const std::string& type)
{
    return owner + kDot + PartLabel(part) + (type.empty() ? std::string() : kDot + TypeLabel(part, type));
}

}  // namespace

// ---- Targets -----------------------------------------------------------------------------------

std::string LeafKey(int block)
{
    return "b" + std::to_string(block);
}

bool BlockOfKey(const std::string& key, int* block)
{
    if (key.size() < 2 || key[0] != 'b') return false;
    int v = 0;
    for (size_t i = 1; i < key.size(); ++i) {
        if (key[i] < '0' || key[i] > '9') return false;
        v = v * 10 + (key[i] - '0');
        if (v > 1000000) return false;
    }
    if (block) *block = v;
    return true;
}

const Parent* Targets::ParentOf(const std::string& leaf_key) const
{
    for (const std::vector<Parent>* list : {&rules, &autos})
        for (const Parent& p : *list)
            for (const Leaf& l : p.leaves)
                if (l.key == leaf_key) return &p;
    return nullptr;
}

const Leaf* Targets::FindLeaf(const std::string& leaf_key) const
{
    for (const std::vector<Parent>* list : {&rules, &autos})
        for (const Parent& p : *list)
            for (const Leaf& l : p.leaves)
                if (l.key == leaf_key) return &l;
    return nullptr;
}

std::string PairStem(const std::string& marker, char* side)
{
    if (marker.size() < 3) return std::string();
    const char last = marker[marker.size() - 1];
    if ((last != 'L' && last != 'R') || marker[marker.size() - 2] != ' ') return std::string();
    if (side) *side = last;
    std::string stem = marker.substr(0, marker.size() - 2);
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    return stem;
}

Targets BuildTargets(const std::vector<Block>& blocks)
{
    Targets out;
    // Rules: a marker pair "<stem> L" / "<stem> R" is one parent, any other rule its own.
    std::map<std::string, int> sides;  // stem -> bit 1 = L seen, 2 = R seen
    for (const Block& b : blocks) {
        char        side = 0;
        const std::string stem = IsAutoBlock(b) ? std::string() : PairStem(b.marker, &side);
        if (!stem.empty()) sides[stem] |= side == 'L' ? 1 : 2;
    }
    std::map<std::string, size_t> parent_of_stem;
    std::map<std::string, size_t> auto_parent;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const Block& b = blocks[i];
        Leaf         leaf;
        leaf.key = LeafKey(static_cast<int>(i));
        leaf.name = b.marker.empty() ? std::string("(unnamed)") : b.marker;
        leaf.block = static_cast<int>(i);
        leaf.rid = b.rid;
        out.signature += std::to_string(i) + ":" + b.marker + ":" + b.auto_type + ";";
        if (IsAutoBlock(b)) {
            std::string word = "?", name = b.auto_type.empty() ? std::string("Auto") : b.auto_type;
            AutoKind    kind;
            char        side;
            if (AutoBlockKind(b, &kind, &side)) {
                word = AutoTypeWord(AutoTypeOfKind(kind));
                std::string part, type;
                PartOfBlock(b, &part, &type);
                name = TypeLabel(part, word);
            }
            auto it = auto_parent.find(word);
            if (it == auto_parent.end()) {
                Parent p;
                p.id = "a:" + word;
                p.name = name;
                p.is_auto = true;
                out.autos.push_back(p);
                it = auto_parent.emplace(word, out.autos.size() - 1).first;
            }
            out.autos[it->second].leaves.push_back(leaf);
            continue;
        }
        char              side = 0;
        const std::string stem = PairStem(b.marker, &side);
        if (!stem.empty() && sides[stem] == 3) {
            auto it = parent_of_stem.find(stem);
            if (it == parent_of_stem.end()) {
                Parent p;
                p.id = "r:" + leaf.key;
                p.name = stem;
                out.rules.push_back(p);
                it = parent_of_stem.emplace(stem, out.rules.size() - 1).first;
            }
            out.rules[it->second].leaves.push_back(leaf);
        } else {
            Parent p;
            p.id = "r:" + leaf.key;
            p.name = leaf.name;
            p.leaves.push_back(leaf);
            out.rules.push_back(p);
        }
    }
    // A pair reads L first.
    for (Parent& p : out.rules)
        if (p.leaves.size() > 1) {
            std::stable_sort(p.leaves.begin(), p.leaves.end(), [](const Leaf& a, const Leaf& b) {
                return a.name.back() == 'L' && b.name.back() != 'L';
            });
        }
    return out;
}

// ---- One target ----------------------------------------------------------------------------------

const GroupState* Target::Group(const std::string& group_uid) const
{
    for (const GroupState& g : groups)
        if (g.group_uid == group_uid) return &g;
    return nullptr;
}

namespace {

// The nodes an event of these layers can reach (review 13-15b): every child of a Random (any can
// be drawn) or a Blend (all play), the case a Switch plays for these values (its default when the
// value has no case). Not ResolveEvent's walk: that follows one Random child only.
void ReachWalk(const SoundStore& store, const SoundLibrary& lib, const ValueLayers& layers, const SoundNode& n,
               std::set<std::string>* seen)
{
    if (!seen->insert(n.uid).second) return;
    auto child_of_case = [&](const std::string& value) -> const SoundNode* {
        for (const SwitchCase& c : n.cases)
            if (c.value_uid == value) return FindNode(lib, c.child_uid);
        return nullptr;
    };
    switch (n.type) {
    case SoundNodeType::Sound: return;
    case SoundNodeType::Random:
    case SoundNodeType::Blend:
        for (const SoundNode* c : ChildrenOf(lib, n.uid))
            if (c) ReachWalk(store, lib, layers, *c, seen);
        return;
    case SoundNodeType::Switch: {
        ValueFrom         from = ValueFrom::Default;
        const std::string v = EffectiveValue(n, layers, &from);
        const std::string def = DefaultValue(store, n);
        const SoundNode*  c = from == ValueFrom::Default ? child_of_case(def) : child_of_case(v);
        if (!c && from != ValueFrom::Default) c = child_of_case(def);
        if (c) ReachWalk(store, lib, layers, *c, seen);
        return;
    }
    }
}

// The groups of `lib` as a target with these layers shows them. `own` = the target's own layer values;
// `below` = the layers under it (what inherit gives).
void FillGroups(const SoundStore& store, const SoundLibrary& lib, const EffectiveAssign& ea,
                const std::vector<ValueSet>& own, bool item_row, bool blocked, Target* t)
{
    std::set<std::string> path, reached_groups;
    if (!blocked)
        if (const SoundNode* root = FindNode(lib, lib.root)) {
            ReachWalk(store, lib, ea.layers, *root, &path);
            for (const std::string& n : path)
                if (const SoundNode* nd = FindNode(lib, n))
                    if (nd->type == SoundNodeType::Switch) reached_groups.insert(nd->group_uid);
        }
    for (const GroupUse& use : GroupsUsed(store, lib)) {
        GroupState gs;
        gs.group_uid = use.group_uid;
        gs.first_switch = use.first_switch;
        const SwitchGroup* g = FindGroup(store, use.group_uid);
        gs.deleted = g == nullptr;
        if (g) gs.group_name = g->name;
        if (g && use.first_switch) {
            ValueFrom   vf = ValueFrom::Default;
            std::string v = EffectiveValue(*use.first_switch, ea.layers, &vf);
            if (vf == ValueFrom::Default) v = DefaultValue(store, *use.first_switch);
            gs.value_uid = v;
            gs.value_name = ValueNameOf(g, v);
            gs.from = vf == ValueFrom::Default ? std::string("default") : ea.ValueFromText(use.group_uid);
            if (const ValueSet* o = FindSet(own, use.group_uid)) {
                gs.own = true;
                gs.own_uid = o->value_uid;
            }
            // What inherit gives: the layers under the own one (for a rule, the item layer; for the Item row, the default).
            ValueLayers below;
            if (!item_row) below.item = ea.layers.item;
            ValueFrom   vf2 = ValueFrom::Default;
            std::string iv = EffectiveValue(*use.first_switch, below, &vf2);
            if (vf2 == ValueFrom::Default) iv = DefaultValue(store, *use.first_switch);
            gs.inherit_name = ValueNameOf(g, iv);
            gs.inherit_from = "default";
            if (vf2 != ValueFrom::Default) {
                gs.inherit_from = "item";
                for (const auto& f : ea.item_from)
                    if (f.first == use.group_uid) gs.inherit_from = f.second;
            }
            if (!blocked && !reached_groups.count(use.group_uid)) {
                gs.reached = false;
                gs.not_reached = NotReachedText(store, lib, path, *use.first_switch);
            }
        }
        t->groups.push_back(gs);
    }
}

}  // namespace

Target MakeTarget(const SoundStore& store, const ItemSounds& rec, const SoundCharacter* ch, const Block& b,
                  const std::string& key, const std::string& title)
{
    Target t;
    t.key = key;
    t.title = title;
    PartOfBlock(b, &t.part, &t.type);
    t.ea = ResolveBlock(store, rec, ch, b);
    t.inherited = ResolveAssign(store, ItemSounds(), ch, std::string(), t.part, t.type);
    const RuleSounds* rs = b.rid.empty() ? nullptr : FindRuleSounds(rec, b.rid);
    if (rs) {
        t.own_state = rs->none ? LibState::None : rs->library.empty() ? LibState::Inherit : LibState::Library;
        t.own_library = rs->library;
    }
    t.lib = t.ea.library.empty() ? nullptr : FindLibrary(store, t.ea.library);
    if (t.lib) {
        if (!HasSound(*t.lib)) {
            t.lib_empty = true;
        } else {
            const LibraryCheck chk = CheckLibrary(store, *t.lib);
            if (chk.blocked) {
                t.lib_blocked = true;
                t.blocked_reason = BlockedText(store, *t.lib, chk);
            }
        }
        if (!t.lib_empty) FillGroups(store, *t.lib, t.ea, rs ? rs->values : std::vector<ValueSet>(), false, t.lib_blocked, &t);
    }
    return t;
}

Target MakeItemTarget(const SoundStore& store, const ItemSounds& rec, const std::vector<Target>& others)
{
    Target t;
    t.key = kItemKey;
    t.title = "Item values";
    t.is_item = true;
    t.ea.layers.item = rec.item_values;
    std::set<std::string> used;
    std::map<std::string, const GroupState*> first;
    for (const Target& o : others)
        for (const GroupState& g : o.groups)
            if (!g.deleted && g.first_switch && !first.count(g.group_uid)) {
                used.insert(g.group_uid);
                first[g.group_uid] = &g;
            }
    for (const SwitchGroup& sg : store.groups) {
        if (!used.count(sg.uid)) continue;
        const GroupState* src = first[sg.uid];
        GroupState        gs;
        gs.group_uid = sg.uid;
        gs.group_name = sg.name;
        gs.first_switch = src->first_switch;
        ValueFrom   vf = ValueFrom::Default;
        std::string v = EffectiveValue(*src->first_switch, t.ea.layers, &vf);
        if (vf == ValueFrom::Default) v = DefaultValue(store, *src->first_switch);
        gs.value_uid = v;
        gs.value_name = ValueNameOf(&sg, v);
        gs.from = vf == ValueFrom::Default ? "default" : "item";
        if (const ValueSet* o = FindSet(rec.item_values, sg.uid)) {
            gs.own = true;
            gs.own_uid = o->value_uid;
        }
        gs.inherit_name = ValueNameOf(&sg, DefaultValue(store, *src->first_switch));
        gs.inherit_from = "default";
        t.groups.push_back(gs);
    }
    return t;
}

// ---- Summary ---------------------------------------------------------------------------------------

std::string Summary::Plain() const
{
    std::string s;
    for (const Seg& g : segs) s += g.text;
    return s;
}

Summary Summarize(const SoundStore& store, const std::vector<const Target*>& targets, bool with_lib)
{
    (void)store;
    Summary out;
    if (targets.empty()) return out;
    auto seg = [&](const std::string& text, SegTone tone) { out.segs.push_back({text, tone}); };
    for (const Target* t : targets)
        if (t->lib_empty || t->lib_blocked) out.blocked = true;
    std::set<std::string> libs;
    bool                  any_unset = false, all_inherited = true;
    for (const Target* t : targets) {
        libs.insert(t->ea.none ? "\x01none" : t->ea.library);
        if (t->Unset()) any_unset = true;
        if (t->OwnLib()) all_inherited = false;
    }
    if (libs.size() > 1) {
        seg("mixed libraries", SegTone::Mixed);
        return out;
    }
    const Target& first = *targets[0];
    if (!first.lib) {
        if (any_unset) seg("nothing set", SegTone::Inherited);
        else seg("No sound", all_inherited ? SegTone::Inherited : SegTone::Normal);
        return out;
    }
    if (with_lib) {
        out.has_dot = true;
        out.dot_color = first.lib->color;
        seg(first.lib->name, all_inherited ? SegTone::Inherited : SegTone::Normal);
    }
    if (first.lib_empty) {
        seg(with_lib ? " \xC2\xB7 empty library" : "empty library", SegTone::Inherited);
        return out;
    }
    bool wrote_any = false;
    for (const GroupState& g : first.groups) {
        std::string text;
        SegTone     tone = SegTone::Normal;
        if (g.deleted) {
            text = "! group deleted";
            tone = SegTone::Mixed;
        } else {
            std::vector<const GroupState*> all;
            for (const Target* t : targets) {
                const GroupState* gs = t->Group(g.group_uid);
                if (gs && gs->reached) all.push_back(gs);
            }
            if (all.empty()) continue;
            bool same = true;
            for (const GroupState* x : all)
                if (x->value_uid != all[0]->value_uid || x->own != all[0]->own) same = false;
            if (!same) {
                text = "(per side)";
                tone = SegTone::Mixed;
            } else {
                text = all[0]->value_name;
                tone = all[0]->own ? SegTone::Normal : SegTone::Inherited;
            }
        }
        if (wrote_any) seg(kArrow, SegTone::Faint);
        else if (with_lib) seg(kDot, SegTone::Faint);
        seg(text, tone);
        wrote_any = true;
    }
    return out;
}

Summary SummarizeItem(const SoundStore& store, const Target& item)
{
    (void)store;
    Summary out;
    bool    any = false;
    for (const GroupState& g : item.groups) {
        if (!g.own) continue;
        if (any) out.segs.push_back({kArrow, SegTone::Faint});
        out.segs.push_back({g.value_name, SegTone::Normal});
        any = true;
    }
    if (!any) out.segs.push_back({"defaults", SegTone::Inherited});
    return out;
}

// ---- Mixed -------------------------------------------------------------------------------------------

std::string OwnLibraryArg(const Target& t)
{
    switch (t.own_state) {
    case LibState::None: return kLibNoneWord;
    case LibState::Library: return t.own_library;
    case LibState::Inherit: break;
    }
    return std::string();
}

Mix MixOf(const std::vector<const Target*>& targets)
{
    Mix m;
    if (targets.empty()) return m;
    std::set<std::string> libs, own;
    for (const Target* t : targets) {
        libs.insert(t->ea.none ? "\x01none" : t->ea.library);
        own.insert(OwnLibraryArg(*t));
    }
    m.lib = libs.size() > 1;
    m.own_settings = static_cast<int>(own.size());
    if (!m.lib && targets[0]->lib)
        for (const GroupState& g : targets[0]->groups) {
            if (g.deleted) continue;
            std::set<std::string> vals;
            for (const Target* t : targets)
                if (const GroupState* x = t->Group(g.group_uid)) vals.insert(x->value_uid + (x->own ? "|own" : "|inh"));
            if (vals.size() > 1) m.groups = true;
        }
    return m;
}

// ---- Details text --------------------------------------------------------------------------------------

std::string SelectionTitle(const Targets& tg, const std::vector<const Target*>& sel)
{
    if (sel.empty()) return std::string();
    if (sel.size() == 1) return sel[0]->title;
    const Parent* p = tg.ParentOf(sel[0]->key);
    if (p && p->leaves.size() == sel.size()) {
        bool all = true;
        for (const Target* t : sel)
            if (tg.ParentOf(t->key) != p) all = false;
        if (all) return p->name + (sel.size() == 2 ? std::string(" \xC2\xB7 both sides") : std::string(" \xC2\xB7 all rows"));
    }
    return std::to_string(sel.size()) + " targets";
}

std::string ScopeLine(bool item_row, bool any_bone_rule, const std::string& pool_text, const std::string& character)
{
    if (item_row) return "every rule of this item, unless the rule sets its own";
    std::string s = any_bone_rule ? pool_text : std::string("this item");
    if (!character.empty()) s += std::string(kDot) + "character " + character;
    return s;
}

std::string PlaysText(const Target& t)
{
    if (!t.lib) return "No sound";
    if (t.lib_blocked) return t.lib->name + kArrow + "blocked";
    if (t.lib_empty) return t.lib->name + kArrow + "empty";
    std::string s = t.lib->name;
    for (const GroupState& g : t.groups)
        if (!g.deleted && g.reached) s += kArrow + g.value_name;
    return s;
}

std::vector<PlaysLine> PlaysLines(const std::vector<const Target*>& targets)
{
    std::vector<PlaysLine> out;
    for (const Target* t : targets) {
        const std::string text = PlaysText(*t);
        PlaysLine*        line = nullptr;
        for (PlaysLine& l : out)
            if (l.text == text) line = &l;
        if (!line) {
            out.push_back(PlaysLine());
            line = &out.back();
            line->text = text;
        }
        line->titles.push_back(t->title);
    }
    return out;
}

std::string WhyText(const Target& t)
{
    std::string s;
    if (!t.Plays()) return s;
    for (const GroupState& g : t.groups) {
        if (g.deleted || !g.reached) continue;
        if (!s.empty()) s += kDot;
        s += g.value_name + ": " + g.from;
    }
    return s;
}

std::string TrackNameOf(const Target& t)
{
    std::string s;
    for (const GroupState& g : t.groups) {
        if (g.deleted || !g.reached) continue;
        if (!s.empty()) s += "-";
        s += g.value_name;
    }
    return s;
}

int EventOverrideCount(const ItemSounds& rec, const std::vector<std::string>& rids, const std::string& group_uid)
{
    int n = 0;
    for (const EventSounds& e : rec.events) {
        if (std::find(rids.begin(), rids.end(), e.rid) == rids.end()) continue;
        if (FindSet(e.overrides, group_uid)) ++n;
    }
    return n;
}

Ladder BuildLadder(const SoundStore& store, const SoundCharacter* ch, const Target& t)
{
    Ladder out;
    out.part_label = PartLabel(t.part);
    for (const SoundLibrary& l : store.libraries)
        if (KitServes(l, t.part)) out.kits.push_back(l.name);
    auto level = [&](const std::string& owner, const std::vector<SoundAssign>& list, const std::string& key,
                     const std::string& type) {
        LadderRow r;
        r.label = SourceLabel(owner, t.part, type);
        if (const SoundAssign* a = FindAssign(list, key))
            if (a->state != LibState::Inherit) {
                r.set = true;
                r.none = a->state == LibState::None;
                r.library = a->library;
                r.kit = a->kit;
            }
        out.rows.push_back(r);
    };
    level("Project", store.project, t.part, std::string());
    level("Project", store.project, AssignKey(t.part, t.type), t.type);
    if (ch) {
        const std::string name = ch->name.empty() ? std::string("Character") : ch->name;
        level(name, ch->assigns, t.part, std::string());
        level(name, ch->assigns, AssignKey(t.part, t.type), t.type);
    }
    LadderRow rule;
    rule.label = t.type == kTypeRules ? "this rule" : "this type";
    rule.set = t.own_state != LibState::Inherit;
    rule.none = t.own_state == LibState::None;
    rule.library = t.own_library;
    out.rows.push_back(rule);
    for (LadderRow& r : out.rows) r.hit = r.set && r.label == t.ea.from;
    return out;
}

// ---- Overview ----------------------------------------------------------------------------------------

std::vector<std::string> OverviewGroups(const SoundStore& store, const std::vector<Target>& targets)
{
    std::set<std::string> used;
    for (const Target& t : targets)
        if (t.lib && !t.lib_empty)
            for (const GroupState& g : t.groups)
                if (!g.deleted) used.insert(g.group_uid);
    std::vector<std::string> out;
    for (const SwitchGroup& g : store.groups)
        if (used.count(g.uid)) out.push_back(g.uid);
    return out;
}

Cell OverviewCell(const Target& t, const std::string& group_uid)
{
    Cell c;
    const GroupState* g = t.Group(group_uid);
    if (!t.lib || t.lib_empty || !g || g->deleted) return c;
    if (!g->reached) {
        c.kind = CellKind::NotReached;
        c.text = "not reached";
        return c;
    }
    c.kind = CellKind::Value;
    c.text = g->value_name;
    c.inherited = !g->own;
    return c;
}

// ---- Last change ---------------------------------------------------------------------------------------

ChangeText LastChange(const std::string& undo_name, const std::string& where, const std::string& what, int targets)
{
    ChangeText c;
    c.prefix = "Undo point ";
    c.bold = "RAV: " + undo_name;
    c.rest = std::string(kDot) + where + kDot + what + kDot + std::to_string(targets) + (targets == 1 ? " target" : " targets");
    return c;
}

// ---- Selection -----------------------------------------------------------------------------------------

bool Selection::Has(const std::string& key) const
{
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

void Selection::Clear()
{
    keys.clear();
    anchor.clear();
    cursor.clear();
}

RowKind KindOf(const std::string& key)
{
    return key == kItemKey ? RowKind::Item : RowKind::Rule;
}

void ClickRow(Selection& s, const std::string& key, bool shift, bool ctrl, const std::vector<std::string>& visible)
{
    const RowKind kind = KindOf(key);
    s.cursor = key;
    if (shift && !s.anchor.empty() && KindOf(s.anchor) == kind) {
        const auto a = std::find(visible.begin(), visible.end(), s.anchor);
        const auto b = std::find(visible.begin(), visible.end(), key);
        if (a != visible.end() && b != visible.end()) {
            const auto lo = std::min(a, b), hi = std::max(a, b);
            s.keys.clear();
            for (auto it = lo; it <= hi; ++it)
                if (KindOf(*it) == kind) s.keys.push_back(*it);
            return;
        }
    }
    if (ctrl) {
        bool same = true;
        for (const std::string& k : s.keys)
            if (KindOf(k) != kind) same = false;
        if (same) {
            const auto it = std::find(s.keys.begin(), s.keys.end(), key);
            if (it != s.keys.end()) s.keys.erase(it);
            else s.keys.push_back(key);
            s.anchor = key;
            return;
        }
    }
    s.keys.assign(1, key);
    s.anchor = key;
}

void ClickParent(Selection& s, const std::vector<std::string>& leaf_keys, bool toggle)
{
    if (leaf_keys.empty()) return;
    s.keys.erase(std::remove(s.keys.begin(), s.keys.end(), std::string(kItemKey)), s.keys.end());
    if (toggle) {
        bool all = true;
        for (const std::string& k : leaf_keys)
            if (!s.Has(k)) all = false;
        for (const std::string& k : leaf_keys) {
            const auto it = std::find(s.keys.begin(), s.keys.end(), k);
            if (all) {
                if (it != s.keys.end()) s.keys.erase(it);
            } else if (it == s.keys.end()) {
                s.keys.push_back(k);
            }
        }
    } else {
        s.keys = leaf_keys;
    }
    s.anchor = leaf_keys[0];
    s.cursor = leaf_keys[0];
}

int ParentState(const Selection& s, const std::vector<std::string>& leaf_keys)
{
    int n = 0;
    for (const std::string& k : leaf_keys)
        if (s.Has(k)) ++n;
    return n == 0 ? 0 : n == static_cast<int>(leaf_keys.size()) ? 1 : 2;
}

void Prune(Selection& s, const std::vector<std::string>& valid)
{
    s.keys.erase(std::remove_if(s.keys.begin(), s.keys.end(),
                                [&](const std::string& k) { return std::find(valid.begin(), valid.end(), k) == valid.end(); }),
                 s.keys.end());
    if (!s.anchor.empty() && std::find(valid.begin(), valid.end(), s.anchor) == valid.end()) s.anchor.clear();
    if (!s.cursor.empty() && std::find(valid.begin(), valid.end(), s.cursor) == valid.end()) s.cursor.clear();
}

bool FilterMatch(const std::string& filter, const std::string& text)
{
    if (filter.empty()) return true;
    return Lower(text).find(Lower(filter)) != std::string::npos;
}

}  // namespace assign
}  // namespace rav
