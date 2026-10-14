// SPDX-License-Identifier: MIT
//
// See sound_populate_plan.h.

#include "sound_populate_plan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "sound_draw.h"
#include "sound_place_plan.h"
#include "sound_record.h"

namespace rav {

namespace {

const char kDot[] = " \xC2\xB7 ";  // " · "

bool IsLive(const SoundPlacement& p)
{
    return p.own == Ownership::Populated;
}

void WalkGroups(const SoundStore& store, const SoundLibrary& lib, const SoundNode& n, std::set<std::string>& seen_nodes,
                std::vector<GroupUse>& out)
{
    if (!seen_nodes.insert(n.uid).second) return;
    if (n.type == SoundNodeType::Switch && !n.group_uid.empty()) {
        bool have = false;
        for (const GroupUse& g : out)
            if (g.group_uid == n.group_uid) have = true;
        if (!have) out.push_back({n.group_uid, &n});
    }
    (void)store;
    for (const SoundNode* c : ChildrenOf(lib, n.uid)) WalkGroups(store, lib, *c, seen_nodes, out);
}

const SoundDraw* FindDraw(const std::vector<SoundDraw>* draws, const std::string& node)
{
    if (!draws) return nullptr;
    for (const SoundDraw& d : *draws)
        if (d.node == node) return &d;
    return nullptr;
}

int Draw(PopulateDrawState& st, EventDraw& ev, const SoundNode& n, const std::vector<int>& active, int avoid,
         const std::string& part = std::string())
{
    const SoundDraw* prev = FindDraw(ev.prev, n.uid);
    std::vector<int>& hist = st.history[part.empty() ? n.uid : n.uid + "|" + part];  // 13-23: per part
    const int pick = KeepOrDraw(prev, active, avoid, hist, st.seed, NodeSalt(ev.rid, ev.t, n.uid));
    if (pick < 0) return -1;
    hist.push_back(pick);
    SoundDraw d;
    d.node = n.uid;
    d.pick = pick;
    d.part = part;
    if (prev && prev->pick == pick) {
        d.hand = prev->hand;
        d.kept = prev->kept;
    }
    for (SoundDraw& o : ev.out)
        if (o.node == n.uid) {
            o = d;
            return pick;
        }
    ev.out.push_back(d);
    return pick;
}

}  // namespace

OrphanPrune PruneOrphanRules(ItemSounds& item, const std::set<std::string>& live_rids)
{
    OrphanPrune r;
    for (size_t i = 0; i < item.rules.size();) {
        if (!live_rids.count(item.rules[i].rid)) {
            r.rules.push_back(item.rules[i]);
            item.rules.erase(item.rules.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
    for (size_t i = 0; i < item.events.size();) {
        EventSounds& ev = item.events[i];
        if (live_rids.count(ev.rid)) {
            ++i;
            continue;
        }
        std::vector<SoundPlacement> keep;
        for (const SoundPlacement& p : ev.placed) {
            if (p.own == Ownership::Populated) r.remove_guids.push_back(p.item_guid);
            else if (p.own != Ownership::Deleted) keep.push_back(p);
        }
        ev.placed = keep;
        if (ev.placed.empty()) item.events.erase(item.events.begin() + static_cast<long>(i));
        else ++i;
    }
    return r;
}

std::string RuleGoneText(const std::string& library_name)
{
    return "Rule gone: library \"" + library_name + "\" unassigned";
}

ValueLayers LayersFor(const ItemSounds& item, const std::string& rid, double t)
{
    ValueLayers l;
    l.item = item.item_values;
    if (const RuleSounds* r = FindRuleSounds(item, rid)) l.rule = r->values;
    if (const EventSounds* e = FindEventSounds(item, rid, t)) l.event = e->overrides;
    return l;
}

bool SetValueIn(std::vector<ValueSet>& values, const std::string& group, const std::string& value)
{
    for (size_t i = 0; i < values.size(); ++i) {
        if (values[i].group_uid != group) continue;
        if (value.empty()) {
            values.erase(values.begin() + static_cast<long>(i));
            return true;
        }
        if (values[i].value_uid == value) return false;
        values[i].value_uid = value;
        return true;
    }
    if (value.empty()) return false;
    ValueSet v;
    v.group_uid = group;
    v.value_uid = value;
    values.push_back(v);
    return true;
}

std::vector<GroupUse> GroupsUsed(const SoundStore& store, const SoundLibrary& lib)
{
    std::vector<GroupUse> out;
    const SoundNode* root = FindNode(lib, lib.root);
    if (!root) return out;
    std::set<std::string> seen;
    WalkGroups(store, lib, *root, seen, out);
    return out;
}

uint64_t NodeSalt(const std::string& rid, double t, const std::string& node_uid)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : node_uid) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return EventSalt(rid, t) ^ h;
}

ResolveHooks MakeDrawHooks(PopulateDrawState& state, EventDraw& ev, std::function<bool(const SoundNode&)> is_missing)
{
    ResolveHooks h;
    PopulateDrawState* st = &state;
    EventDraw* e = &ev;
    h.random_child = [st, e](const SoundNode& n) -> int {
        if (!st->lib) return -1;
        const size_t kids = ChildrenOf(*st->lib, n.uid).size();
        std::vector<int> active;
        for (size_t i = 0; i < kids; ++i) active.push_back(static_cast<int>(i));
        return Draw(*st, *e, n, active, n.avoid_repeat);
    };
    h.variation = [st, e](const SoundNode& n) -> int {
        if (n.sound.frames.empty()) return -1;
        const std::vector<int> active = ActiveVariations(n.sound);
        if (active.empty()) {
            e->no_active.insert(n.uid);
            return -1;
        }
        return Draw(*st, *e, n, active, n.avoid_repeat);
    };
    h.part_variation = [st, e](const SoundNode& n, const std::string& part) -> int {
        if (n.sound.frames.empty()) return -1;
        if (ActiveVariations(n.sound).empty()) {
            e->no_active.insert(n.uid);
            return -1;
        }
        // Takes mode places the take whole (A8): every on take can play.
        const std::vector<int> active =
            n.sound.mode == DetectMode::Takes ? ActiveVariations(n.sound) : ActiveVariationsFor(n.sound, part);
        if (active.empty()) {
            e->no_part[n.uid] = part;
            return -1;
        }
        return Draw(*st, *e, n, active, n.avoid_repeat, part);
    };
    h.is_missing = std::move(is_missing);
    return h;
}

// ---- 13-59: the variation of each loop pass ----------------------------------------------------

namespace {

constexpr const char* kLoopVarKey = "loopvar=";
constexpr const char* kPassKeyMark = "#p";

bool IsLoopVarToken(const std::string& tok)
{
    return tok.compare(0, std::strlen(kLoopVarKey), kLoopVarKey) == 0;
}

}  // namespace

LoopPassDraw LoopPassDrawOf(const SoundStore& store)
{
    for (const std::string& tok : store.kept)
        if (IsLoopVarToken(tok)) return tok.substr(std::strlen(kLoopVarKey)) == "same" ? LoopPassDraw::Same : LoopPassDraw::New;
    return LoopPassDraw::New;
}

bool SetLoopPassDraw(SoundStore& store, LoopPassDraw draw)
{
    if (LoopPassDrawOf(store) == draw) return false;
    std::vector<std::string> kept;
    for (const std::string& tok : store.kept)
        if (!IsLoopVarToken(tok)) kept.push_back(tok);
    if (draw == LoopPassDraw::Same) kept.push_back(std::string(kLoopVarKey) + "same");
    store.kept = kept;
    return true;
}

std::string PassDrawKey(const std::string& node_uid, long long pass)
{
    return node_uid + kPassKeyMark + std::to_string(pass);
}

bool ParsePassDrawKey(const std::string& key, long long* pass, std::string* node_uid)
{
    const size_t at = key.rfind(kPassKeyMark);
    if (at == std::string::npos || at == 0 || at + 2 >= key.size()) return false;
    long long v = 0;
    bool      neg = false;
    size_t    i = at + 2;
    if (key[i] == '-') {
        neg = true;
        ++i;
    }
    if (i >= key.size()) return false;
    for (; i < key.size(); ++i) {
        if (key[i] < '0' || key[i] > '9') return false;
        if (v > 100000000000LL) return false;
        v = v * 10 + (key[i] - '0');
    }
    if (pass) *pass = neg ? -v : v;
    if (node_uid) *node_uid = key.substr(0, at);
    return true;
}

uint64_t PassDrawSeed(uint64_t seed, long long pass)
{
    if (pass == 0) return seed;
    uint64_t z = static_cast<uint64_t>(pass) + 0x9E3779B97F4A7C15ull;  // splitmix64 of the pass id
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    const uint64_t s = seed ^ z;
    return s ? s : 0x9E3779B97F4A7C15ull;
}

std::vector<SoundDraw> PassPrevDraws(const std::vector<SoundDraw>& draws, long long pass)
{
    std::vector<SoundDraw> out;
    for (const SoundDraw& d : draws)
        if (d.hand && !ParsePassDrawKey(d.node, nullptr, nullptr)) out.push_back(d);
    for (const SoundDraw& d : draws) {
        long long   p = 0;
        std::string node;
        if (!ParsePassDrawKey(d.node, &p, &node) || p != pass) continue;
        SoundDraw x = d;
        x.node = node;
        x.hand = false;
        out.push_back(x);
    }
    return out;
}

void MergePassDraws(std::vector<SoundDraw>& draws, long long pass, const std::vector<SoundDraw>& out)
{
    draws.erase(std::remove_if(draws.begin(), draws.end(),
                               [pass](const SoundDraw& d) {
                                   long long p = 0;
                                   return ParsePassDrawKey(d.node, &p, nullptr) && p == pass;
                               }),
                draws.end());
    for (const SoundDraw& d : out) {
        SoundDraw x = d;
        x.node = PassDrawKey(d.node, pass);
        x.hand = false;
        draws.push_back(x);
    }
}

bool HasPassDraws(const std::vector<SoundDraw>& draws, long long pass)
{
    for (const SoundDraw& d : draws) {
        long long p = 0;
        if (ParsePassDrawKey(d.node, &p, nullptr) && p == pass) return true;
    }
    return false;
}

std::vector<SoundDraw> PassDrawsFromPlaced(const SoundLibrary& lib, const std::vector<PlacedPick>& placed)
{
    std::vector<SoundDraw> out;
    auto have = [&out](const std::string& uid) {
        for (const SoundDraw& d : out)
            if (d.node == uid) return true;
        return false;
    };
    for (const PlacedPick& pk : placed) {
        const SoundNode* n = FindNode(lib, pk.node);
        if (!n) continue;
        if (pk.variation >= 0 && !have(n->uid)) {
            SoundDraw d;
            d.node = n->uid;
            d.pick = pk.variation;
            d.part = pk.part;
            out.push_back(d);
        }
        // Up to the root: each Random ancestor's child on the way (a guard against a parent loop).
        const SoundNode* child = n;
        for (int depth = 0; depth < 64 && child && !child->parent.empty(); ++depth) {
            const SoundNode* up = FindNode(lib, child->parent);
            if (!up) break;
            if (up->type == SoundNodeType::Random && !have(up->uid)) {
                const std::vector<const SoundNode*> kids = ChildrenOf(lib, up->uid);
                for (size_t i = 0; i < kids.size(); ++i)
                    if (kids[i]->uid == child->uid) {
                        SoundDraw d;
                        d.node = up->uid;
                        d.pick = static_cast<int>(i);
                        out.push_back(d);
                        break;
                    }
            }
            child = up;
        }
    }
    return out;
}

void PrunePassDraws(std::vector<SoundDraw>& draws, const std::set<long long>& keep)
{
    draws.erase(std::remove_if(draws.begin(), draws.end(),
                               [&keep](const SoundDraw& d) {
                                   long long p = 0;
                                   return ParsePassDrawKey(d.node, &p, nullptr) && !keep.count(p);
                               }),
                draws.end());
}

std::vector<SlotStep> PlanPassFill(const std::vector<DesiredSlot>& desired, const std::vector<ExistingSlot>& existing)
{
    std::vector<SlotStep> out;
    for (const SlotStep& st : PlanEventSlots(desired, existing))
        if (st.act == SlotAction::Place) out.push_back(st);
    return out;
}

std::vector<SlotStep> PlanEventSlots(const std::vector<DesiredSlot>& desired, const std::vector<ExistingSlot>& existing)
{
    std::vector<SlotStep> steps;
    // A node deleted by hand is never placed again, but its copies still live on other passes (or
    // the rest of its pile) are kept: the deletion only blocks a new placement.
    std::set<std::string> deleted;
    for (const ExistingSlot& x : existing)
        if (x.own == Ownership::Deleted) deleted.insert(x.node);
    std::set<int> passes;
    for (const DesiredSlot& d : desired) passes.insert(d.pass);
    for (const ExistingSlot& x : existing)
        if (x.own == Ownership::Populated) passes.insert(x.pass);
    for (int pass : passes) {
        // The existing slots of this pass: Populated items grouped by node, in first-seen order.
        std::vector<std::pair<std::string, std::vector<int>>> slots;
        for (size_t i = 0; i < existing.size(); ++i) {
            const ExistingSlot& x = existing[i];
            if (x.own != Ownership::Populated || x.pass != pass) continue;
            bool added = false;
            for (auto& s : slots)
                if (s.first == x.node) {
                    s.second.push_back(static_cast<int>(i));
                    added = true;
                }
            if (!added) slots.push_back({x.node, {static_cast<int>(i)}});
        }
        std::vector<int> want;
        for (size_t i = 0; i < desired.size(); ++i)
            if (desired[i].pass == pass) want.push_back(static_cast<int>(i));
        std::vector<char> slot_used(slots.size(), 0);
        std::vector<int>  unmatched;
        for (int di : want) {
            bool matched = false;
            for (size_t s = 0; s < slots.size(); ++s) {
                if (slot_used[s] || slots[s].first != desired[static_cast<size_t>(di)].node) continue;
                slot_used[s] = 1;
                SlotStep st;
                st.desired = di;
                st.existing = slots[s].second;
                const ExistingSlot& first = existing[static_cast<size_t>(st.existing.front())];
                const DesiredSlot&  want_d = desired[static_cast<size_t>(di)];
                const bool same = first.variation == want_d.variation && first.part == want_d.part &&
                                  // a piece deleted by hand is not placed again: its count is not compared
                                  (want_d.pieces <= 0 || deleted.count(want_d.node) ||
                                   static_cast<int>(st.existing.size()) == want_d.pieces) &&
                                  (want_d.sync < 0.0 || (first.sync >= 0.0 && std::fabs(first.sync - want_d.sync) <= 1e-6));
                st.act = same ? SlotAction::Keep : SlotAction::SetVariation;
                steps.push_back(st);
                matched = true;
                break;
            }
            if (!matched) unmatched.push_back(di);
        }
        for (int di : unmatched) {
            if (deleted.count(desired[static_cast<size_t>(di)].node)) continue;
            SlotStep st;
            st.desired = di;
            for (size_t s = 0; s < slots.size(); ++s)
                if (!slot_used[s]) {
                    slot_used[s] = 1;
                    st.existing = slots[s].second;
                    break;
                }
            st.act = st.existing.empty() ? SlotAction::Place : SlotAction::ReplaceNode;
            steps.push_back(st);
        }
        for (size_t s = 0; s < slots.size(); ++s)
            if (!slot_used[s]) {
                SlotStep st;
                st.act = SlotAction::Remove;
                st.existing = slots[s].second;
                steps.push_back(st);
            }
    }
    return steps;
}

int NearestPass(const std::vector<double>& pass_times, double pos)
{
    int    best = -1;
    double best_d = 0.0;
    for (size_t i = 0; i < pass_times.size(); ++i) {
        const double d = std::fabs(pass_times[i] - pos);
        if (best < 0 || d < best_d) {
            best = static_cast<int>(i);
            best_d = d;
        }
    }
    return best;
}

double NudgeOf(double item_pos, double project_t, double sync_offset, double blend_ms)
{
    return item_pos - PlacementPosition(project_t, sync_offset, blend_ms, 0.0);
}

double BlendOffsetMs(const SoundLibrary& lib, const std::string& node_uid, const std::string& role)
{
    double                ms = 0.0;
    std::set<std::string> seen;
    const SoundNode*      n = FindNode(lib, node_uid);
    while (n && !n->parent.empty() && seen.insert(n->uid).second) {
        const SoundNode* p = FindNode(lib, n->parent);
        if (p && p->type == SoundNodeType::Blend) {
            // 13-31 (a): on an event with a role, a role-using Blend drops its layer's offset (unless kept),
            // as ResolveEvent does (BlendUsesRoles, inlined: this file links without sound_switch).
            bool uses_roles = false;
            if (!role.empty() && !n->layer_keep_offset)
                for (const SoundNode* c : ChildrenOf(lib, p->uid)) uses_roles |= !c->layer_roles.empty();
            if (!uses_roles) ms += n->offset_ms;
        }
        n = p;
    }
    return ms;
}

int NearestPassOfItem(const std::vector<double>& pass_times, double item_pos, double sync_offset, double blend_ms)
{
    return NearestPass(pass_times, item_pos + sync_offset - blend_ms / 1000.0);
}

int MarkDeletedPlacements(EventSounds& ev, const std::function<bool(const std::string&)>& exists)
{
    int n = 0;
    for (SoundPlacement& p : ev.placed)
        if (p.own == Ownership::Populated && exists && !exists(p.item_guid)) {
            p.own = Ownership::Deleted;
            ++n;
        }
    return n;
}

bool EventSkippedByPopulate(const EventSounds* ev)
{
    return ev && EventHasUserPlacement(*ev);
}

bool EventHasUserPlacement(const EventSounds& ev)
{
    for (const SoundPlacement& p : ev.placed)
        if (p.own == Ownership::Attached || p.own == Ownership::Spread) return true;
    return false;
}

bool EventIsPopulated(const EventSounds& ev)
{
    for (const SoundPlacement& p : ev.placed)
        if (IsLive(p)) return true;
    return false;
}

int CountEvents(const std::vector<int>& event_blocks, const std::vector<char>& rule_ok)
{
    int n = 0;
    for (int b : event_blocks)
        if (b >= 0 && static_cast<size_t>(b) < rule_ok.size() && rule_ok[static_cast<size_t>(b)]) ++n;
    return n;
}

std::string NoPartText(const std::string& sound_name, const std::string& value_name)
{
    if (value_name.empty()) return "Nothing placed: " + sound_name + " has nothing left to place";
    return "Nothing placed: " + sound_name + " has no " + value_name + " part left";
}

std::string ReportSummary(const SoundReport& report, int events)
{
    int missing = 0, replaced = 0, gone = 0, blocked = 0, no_sound = 0;
    for (const ReportLine& l : report.lines) {
        switch (l.kind) {
        case ReportKind::Missing: ++missing; break;
        case ReportKind::Replaced: ++replaced; break;
        case ReportKind::Detached: ++gone; break;
        case ReportKind::Blocked: ++blocked; break;
        case ReportKind::Info: break;
        case ReportKind::NoSound: ++no_sound; break;
        }
    }
    char buf[96];
    std::string s;
    if (events >= 0) {
        std::snprintf(buf, sizeof(buf), "Last populate: %d event%s", events, events == 1 ? "" : "s");
        s = buf;
    } else {
        s = "Last change:";
    }
    auto add = [&](int n, const char* one, const char* many) {
        if (n <= 0) return;
        std::snprintf(buf, sizeof(buf), "%d %s", n, n == 1 ? one : many);
        s += (events >= 0 || s.back() != ':') ? kDot : " ";
        s += buf;
    };
    add(missing, "missing", "missing");
    add(no_sound, "no sound", "no sound");
    add(replaced, "replaced", "replaced");
    add(gone, "rule gone", "rules gone");
    add(blocked, "blocked", "blocked");
    if (events < 0 && s.back() == ':') s += " nothing to report";
    return s;
}

}  // namespace rav
