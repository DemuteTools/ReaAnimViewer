// SPDX-License-Identifier: MIT
//
// See sound_sync_plan.h.

#include "sound_sync_plan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "sound_record.h"  // FindNode, FindGroup, ChildrenOf
#include "sound_switch.h"  // DefaultValue

namespace rav {

namespace {

constexpr const char* kMinus = "\xE2\x88\x92";  // U+2212
constexpr const char* kArrow = " \xE2\x86\x92 ";  // " -> "
constexpr const char* kRuleKey = "syncedit=";

// FNV-1a over the bytes of what is added.
struct Hasher {
    uint32_t h = 2166136261u;
    void     Bytes(const void* p, size_t n)
    {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 16777619u;
        }
    }
    void Num(double v)
    {
        if (v == 0.0) v = 0.0;  // -0 reads as 0
        Bytes(&v, sizeof v);
    }
    void Int(long long v) { Bytes(&v, sizeof v); }
    void Str(const std::string& s)
    {
        Int(static_cast<long long>(s.size()));
        Bytes(s.data(), s.size());
    }
    void Opt(const std::optional<double>& v)
    {
        Int(v ? 1 : 0);
        if (v) Num(*v);
    }
    // A hash that is never 0 (0 = a node with no settings of its own).
    uint32_t Get() const { return h ? h : 1u; }
};

std::string Trim1(double v)
{
    char buf[32];
    const double a = std::fabs(v);
    if (std::fabs(a - std::round(a)) < 0.05) std::snprintf(buf, sizeof buf, "%.0f", std::round(a));
    else std::snprintf(buf, sizeof buf, "%.1f", a);
    return buf;
}

// "-3 dB" with the minus sign, "+2 dB", "0 dB".
std::string Signed(double v, const char* unit)
{
    const std::string n = Trim1(v);
    std::string       s = n == "0" ? std::string() : v < 0.0 ? std::string(kMinus) : std::string("+");
    return s + n + unit;
}

// The same without a "+" (a value, not a change).
std::string Value(double v, const char* unit)
{
    const std::string n = Trim1(v);
    return (v < 0.0 && n != "0" ? std::string(kMinus) : std::string()) + n + unit;
}

void Add(std::string& to, const std::string& what)
{
    if (what.empty()) return;
    if (!to.empty()) to += ", ";
    to += what;
}

const SwitchValue* ValueOf(const SoundStore& store, const std::string& group, const std::string& value)
{
    if (const SwitchGroup* g = FindGroup(store, group))
        for (const SwitchValue& v : g->values)
            if (v.uid == value) return &v;
    return nullptr;
}

std::string CaseValueOf(const SoundNode& sw, const std::string& child)
{
    for (const SwitchCase& c : sw.cases)
        if (c.child_uid == child) return c.value_uid;
    return {};
}

// `wr` / `now` (both or neither): the fade sides said by their length now.
std::string HandText(const HandEdit& h, const PlacedWrite* wr, const PlacedWrite* now)
{
    std::string t;
    if (h.vol) Add(t, "volume " + Signed(h.vol_db, " dB"));
    if (h.moved) Add(t, "moved " + Signed(h.nudge_s * 1000.0, " ms"));
    if (h.fades) {
        std::string f;
        if (wr && now) {
            if (std::fabs(wr->fade_in - now->fade_in) > 0.0005) Add(f, "fade-in " + Value(now->fade_in * 1000.0, " ms"));
            if (std::fabs(wr->fade_out - now->fade_out) > 0.0005) Add(f, "fade-out " + Value(now->fade_out * 1000.0, " ms"));
        }
        Add(t, f.empty() ? std::string(wr && now ? "fade shape" : "fades") : f);
    }
    if (h.length) Add(t, "length");
    return t;
}

}  // namespace

// ---- Prints ----------------------------------------------------------------------------------

NodePrint PrintOf(const SoundNode& n, int variation)
{
    NodePrint p;
    p.node = n.uid;
    p.vol = n.volume_db;
    p.off = n.offset_ms;
    if (n.type != SoundNodeType::Sound) return p;
    const SoundData& d = n.sound;
    Hasher           fades, sync, zones, level;
    fades.Num(d.fade_in);
    fades.Num(d.fade_out);
    fades.Num(d.cut_fade);
    fades.Num(d.gap_xfade);
    sync.Int(static_cast<int>(d.sync_mode));
    sync.Num(d.sync_offset_ms);
    zones.Int(static_cast<int>(d.mode));
    zones.Num(d.silence.preroll_ms);
    zones.Num(d.silence.tail_ms);
    zones.Str(d.divide_group);
    for (const std::string& v : d.divide_values) zones.Str(v);
    for (const SoundZone& z : d.zones) {
        zones.Num(z.start);
        zones.Num(z.length);
        zones.Str(z.to);
    }
    for (const PartSettings& ps : d.parts) {
        fades.Str(ps.value);
        fades.Opt(ps.fade_in);
        fades.Opt(ps.fade_out);
        sync.Str(ps.value);
        sync.Int(ps.sync_mode ? static_cast<int>(*ps.sync_mode) + 1 : 0);
        sync.Opt(ps.sync_offset_ms);
        zones.Str(ps.value);
        zones.Opt(ps.preroll_ms);
        zones.Opt(ps.tail_ms);
        level.Str(ps.value);
        level.Opt(ps.gain_db);
    }
    if (variation >= 0 && variation < static_cast<int>(d.frames.size())) {
        const SoundFrame& f = d.frames[static_cast<size_t>(variation)];
        p.on = f.on;
        fades.Num(f.fade_in);
        fades.Num(f.fade_out);
        fades.Int(f.fade_in_shape);
        fades.Int(f.fade_out_shape);
        for (double s : f.hand_syncs) sync.Num(s);
        zones.Num(f.start);
        zones.Num(f.length);
        for (double c : f.cuts) zones.Num(c);
        for (const std::string& o : f.owners) zones.Str(o);
        level.Num(f.db);
        for (const auto& kv : f.part_db) {
            level.Str(kv.first);
            level.Num(kv.second);
        }
        for (const EditZone& z : f.zones) {
            zones.Num(z.a);
            zones.Num(z.b);
            zones.Str(z.part);
            zones.Int((z.a_hand ? 1 : 0) | (z.b_hand ? 2 : 0));
            sync.Opt(z.snap);
            fades.Opt(z.fade_in);
            fades.Opt(z.fade_out);
            fades.Int(z.fade_in_shape);
            fades.Int(z.fade_out_shape);
            fades.Opt(z.fade_in_dir);
            fades.Opt(z.fade_out_dir);
            fades.Num(z.xl);
            fades.Num(z.xr);
            level.Num(z.db);
        }
    }
    p.fades = fades.Get();
    p.sync = sync.Get();
    p.zones = zones.Get();
    p.level = level.Get();
    return p;
}

uint32_t StructureOf(const SoundStore& store, const SoundLibrary& lib, const SoundNode& n, const std::string& child)
{
    Hasher h;
    h.Int(static_cast<int>(n.type));
    switch (n.type) {
    case SoundNodeType::Sound: return 0;
    case SoundNodeType::Switch: {
        h.Str(n.group_uid);
        std::vector<std::string> values;
        for (const SwitchCase& c : n.cases)
            if (c.child_uid == child) values.push_back(c.value_uid);
        std::sort(values.begin(), values.end());
        for (const std::string& v : values) h.Str(v);
        h.Int(std::find(n.unassigned.begin(), n.unassigned.end(), child) != n.unassigned.end() ? 1 : 0);
        const std::string def = DefaultValue(store, n);
        bool              plays_default = false;
        for (const SwitchCase& c : n.cases)
            if (c.value_uid == def && c.child_uid == child) plays_default = true;
        h.Int(plays_default ? 1 : 0);
        break;
    }
    case SoundNodeType::Blend:
        for (const SoundNode* c : ChildrenOf(lib, n.uid)) {
            h.Str(c->uid);
            h.Int(static_cast<long long>(c->layer_roles.size()));
            for (const std::string& r : c->layer_roles) h.Str(r);
            h.Int(c->layer_keep_offset ? 1 : 0);
        }
        break;
    case SoundNodeType::Random: {
        h.Int(n.avoid_repeat);
        std::vector<std::string> kids;
        for (const SoundNode* c : ChildrenOf(lib, n.uid)) kids.push_back(c->uid);
        std::sort(kids.begin(), kids.end());  // a Random picks at random: its order shapes nothing
        for (const std::string& k : kids) h.Str(k);
        break;
    }
    }
    return h.Get();
}

std::vector<NodePrint> PathPrints(const SoundStore& store, const SoundLibrary& lib, const std::string& node_uid, int variation)
{
    std::vector<NodePrint> out;
    std::set<std::string>  seen;
    std::string            child;  // the path's child of the node walked
    for (const SoundNode* n = FindNode(lib, node_uid); n && seen.insert(n->uid).second;
         n = n->parent.empty() || n->parent == n->uid ? nullptr : FindNode(lib, n->parent)) {
        NodePrint p = PrintOf(*n, n->uid == node_uid ? variation : -1);
        p.st = StructureOf(store, lib, *n, child);
        out.push_back(p);
        child = n->uid;
    }
    return out;
}

const SoundLibrary* LibraryOfNode(const SoundStore& store, const std::string& uid)
{
    if (uid.empty()) return nullptr;
    for (const SoundLibrary& l : store.libraries)
        if (FindNode(l, uid)) return &l;
    return nullptr;
}

std::vector<std::string> BehindNodes(const PlacedTag& tag, const SoundStore& store)
{
    std::vector<std::string> out;
    if (tag.lib.empty()) return out;
    const SoundLibrary* lib = LibraryOfNode(store, tag.node);
    if (!lib) return out;
    const std::vector<NodePrint> now = PathPrints(store, *lib, tag.node, tag.va);
    if (now.size() != tag.lib.size()) return out;
    for (size_t i = 0; i < now.size(); ++i)
        if (now[i].node != tag.lib[i].node) return {};
    for (size_t i = 0; i < now.size(); ++i)
        if (now[i] != tag.lib[i]) out.push_back(now[i].node);
    return out;
}

const char* ReshapeText(Reshape r)
{
    switch (r) {
    case Reshape::Content: return "content changed (Populate again to update)";
    case Reshape::Moved: return "moved";
    case Reshape::Removed: return "removed";
    default: return "";
    }
}

ReshapeVerdict ReshapeOf(const PlacedTag& tag, const SoundStore& store)
{
    ReshapeVerdict v;
    if (tag.lib.empty()) return v;
    const SoundLibrary* lib = LibraryOfNode(store, tag.node);
    if (!lib) {  // the node gone (or its library): the library's root row
        v.kind = Reshape::Removed;
        const std::string& root = tag.lib.back().node;
        for (const SoundLibrary& l : store.libraries)
            if (l.root == root) v.lib = l.uid;
        v.mark = root;
        return v;
    }
    v.lib = lib->uid;
    const std::vector<NodePrint>& was = tag.lib;
    const std::vector<NodePrint>  now = PathPrints(store, *lib, tag.node, tag.va);
    const size_t                  n = now.size(), m = was.size();
    // From the root down: the first node where the path now leaves the stored one.
    for (size_t i = 0; i < n && i < m; ++i) {
        if (now[n - 1 - i].node == was[m - 1 - i].node) continue;
        v.kind = Reshape::Moved;
        // A container new on the path is the one touched; the sound itself there: the container it now sits in.
        v.mark = i + 1 < n || i == 0 ? now[n - 1 - i].node : now[n - i].node;
        return v;
    }
    if (n != m) {
        v.kind = Reshape::Moved;
        v.mark = now.empty() ? was.back().node : now.back().node;
        return v;
    }
    for (size_t i = n; i-- > 0;)
        if (was[i].st != 0 && was[i].st != now[i].st) {  // st 0 = an older tag: unknown
            v.kind = Reshape::Content;
            v.mark = now[i].node;
            return v;
        }
    return v;
}

std::vector<std::string> ReshapedNodes(const PlacedTag& tag, const SoundStore& store)
{
    const ReshapeVerdict v = ReshapeOf(tag, store);
    if (v.kind == Reshape::None) return {};
    return {v.mark};
}

bool AdoptStructure(std::vector<NodePrint>& stored, const std::vector<NodePrint>& now)
{
    if (now.empty()) return false;
    std::vector<NodePrint> out;
    for (const NodePrint& p : now) {
        NodePrint q = p;
        for (const NodePrint& s : stored)
            if (s.node == p.node) {
                q = s;  // its settings as placed (a setting changed stays behind)
                q.st = p.st;
                break;
            }
        out.push_back(q);
    }
    bool changed = out.size() != stored.size();
    for (size_t i = 0; !changed && i < out.size(); ++i) changed = out[i] != stored[i] || out[i].st != stored[i].st;
    if (changed) stored = out;
    return changed;
}

const NodePrint* TagPrint(const PlacedTag& tag, const std::string& node)
{
    for (const NodePrint& p : tag.lib)
        if (p.node == node) return &p;
    return nullptr;
}

std::string DescribeChange(const NodePrint& was, const NodePrint& now, int variation)
{
    std::string t;
    if (std::fabs(was.vol - now.vol) > 1e-9) Add(t, "Volume " + Value(was.vol, " dB") + kArrow + Value(now.vol, " dB"));
    if (std::fabs(was.off - now.off) > 1e-9) Add(t, "Blend offset " + Value(was.off, "") + kArrow + Value(now.off, " ms"));
    if (was.fades != now.fades) Add(t, "fades");
    if (was.sync != now.sync) Add(t, "sync");
    if (was.zones != now.zones) Add(t, "zones");
    if (was.level != now.level) Add(t, "level");
    if (was.on != now.on) {
        const std::string v = variation >= 0 ? " " + std::to_string(variation + 1) : std::string();
        Add(t, now.on ? "variation" + v + " back on" : "variation" + v + " excluded");
    }
    return t;
}

bool ChangeRedraws(const NodePrint& was, const NodePrint& now) { return was.on && !now.on; }

bool AdoptVolumes(std::vector<NodePrint>& stored, const std::vector<NodePrint>& now)
{
    if (stored.size() != now.size()) return false;
    bool changed = false;
    for (size_t i = 0; i < stored.size(); ++i)
        if (stored[i].node == now[i].node && stored[i].vol != now[i].vol) {
            stored[i].vol = now[i].vol;
            changed = true;
        }
    return changed;
}

void KeptTagAfterVolume(PlacedTag& tag, const std::vector<NodePrint>& now, double vol_was, double vol_new)
{
    if (!tag.lib.empty()) AdoptVolumes(tag.lib, now);
    if (tag.has_wr && vol_was != 0.0 && std::isfinite(vol_was) && std::isfinite(vol_new)) tag.wr.vol *= vol_new / vol_was;
}

SyncStep SyncStepAction(SlotAction act, bool touches_scope)
{
    if (!touches_scope) return SyncStep::Skip;
    switch (act) {
    case SlotAction::Keep: return SyncStep::KeepInPlace;
    case SlotAction::SetVariation: return SyncStep::Replace;
    default: return SyncStep::Skip;  // Place, Remove, ReplaceNode: Populate's
    }
}

EventPlan SyncEventPlan(EventPlan plan, bool has_ev, bool any_behind_in_scope)
{
    if (plan != EventPlan::Draw || any_behind_in_scope) return plan;
    return has_ev ? EventPlan::Keep : EventPlan::Skip;
}

std::string SyncNodePath(const SoundStore& store, const SoundLibrary& lib, const std::string& uid)
{
    std::vector<std::string> parts;
    std::set<std::string>    seen;
    for (const SoundNode* n = FindNode(lib, uid); n && seen.insert(n->uid).second;
         n = n->parent.empty() || n->parent == n->uid ? nullptr : FindNode(lib, n->parent)) {
        if (n->uid == lib.root && n->uid != uid) break;
        const SoundNode* p = n->parent.empty() ? nullptr : FindNode(lib, n->parent);
        std::string      label = n->name;
        const bool       is_case = p && p->type == SoundNodeType::Switch && !CaseValueOf(*p, n->uid).empty();
        if (n->type == SoundNodeType::Switch) {
            if (!is_case && n->uid != uid) continue;  // a Switch says nothing of where a sound is
            if (is_case) {
                const SwitchValue* v = ValueOf(store, p->group_uid, CaseValueOf(*p, n->uid));
                if (v && !v->name.empty()) label = v->name;
            }
        }
        if (label.empty()) label = "(unnamed)";
        if (parts.empty() || parts.back() != label) parts.push_back(label);
    }
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) out += (out.empty() ? "" : " \xE2\x80\xBA ") + *it;
    return out;
}

// ---- Hand edits ------------------------------------------------------------------------------

HandEdit HandEdits(const PlacedWrite& wr, const PlacedWrite& now, double nudge_s)
{
    HandEdit     h;
    const double ms = 0.0005;
    if (wr.vol > 0.0 && now.vol > 0.0 && std::isfinite(wr.vol) && std::isfinite(now.vol)) {
        const double db = 20.0 * std::log10(now.vol / wr.vol);
        if (std::fabs(db) > 0.05) {
            h.vol = true;
            h.vol_db = db;
        }
    } else if (wr.vol != now.vol) {
        h.vol = true;  // a polarity flip or a silenced item
    }
    h.fades = std::fabs(wr.fade_in - now.fade_in) > ms || std::fabs(wr.fade_out - now.fade_out) > ms ||
              wr.in_shape != now.in_shape || wr.out_shape != now.out_shape || std::fabs(wr.in_dir - now.in_dir) > 0.001 ||
              std::fabs(wr.out_dir - now.out_dir) > 0.001;
    h.length = std::fabs(wr.length - now.length) > ms;
    if (std::isfinite(nudge_s) && std::fabs(nudge_s) > ms) {
        h.moved = true;
        h.nudge_s = nudge_s;
    }
    h.text = HandText(h, &wr, &now);
    return h;
}

void MergeHandEdit(HandEdit& a, const HandEdit& b)
{
    if (!b.Any()) return;
    if (!a.Any()) {  // the sound's first edited item: its own text (its fade lengths)
        a = b;
        return;
    }
    if (b.vol && !a.vol) a.vol_db = b.vol_db;
    if (b.moved && !a.moved) a.nudge_s = b.nudge_s;
    a.vol = a.vol || b.vol;
    a.fades = a.fades || b.fades;
    a.length = a.length || b.length;
    a.moved = a.moved || b.moved;
    a.text = HandText(a, nullptr, nullptr);  // several items: their fades said as "fades"
}

HandEditRule HandEditRuleOf(const SoundStore& store)
{
    for (const std::string& tok : store.kept)
        if (tok.compare(0, std::strlen(kRuleKey), kRuleKey) == 0) {
            const std::string v = tok.substr(std::strlen(kRuleKey));
            return v == "keep" ? HandEditRule::Keep : v == "over" ? HandEditRule::Overwrite : HandEditRule::Ask;
        }
    return HandEditRule::Ask;
}

bool SetHandEditRule(SoundStore& store, HandEditRule rule)
{
    if (HandEditRuleOf(store) == rule) return false;
    std::vector<std::string> kept;
    for (const std::string& tok : store.kept)
        if (tok.compare(0, std::strlen(kRuleKey), kRuleKey) != 0) kept.push_back(tok);
    if (rule == HandEditRule::Keep) kept.push_back(std::string(kRuleKey) + "keep");
    if (rule == HandEditRule::Overwrite) kept.push_back(std::string(kRuleKey) + "over");
    store.kept = kept;
    return true;
}

const char* HandEditRuleText(HandEditRule rule)
{
    switch (rule) {
    case HandEditRule::Keep: return "Keep my edits";
    case HandEditRule::Overwrite: return "Overwrite them";
    default: return "Ask each time";
    }
}

SyncItemPlan PlanSyncItem(const PlacedWrite& wr, const PlacedWrite& now, const HandEdit& hand, bool overwrite, double want_db,
                          double g, double nudge_s)
{
    SyncItemPlan p;
    double       k = std::pow(10.0, (want_db - g) / 20.0);
    if (!std::isfinite(k)) k = 1.0;
    p.wr_vol = wr.vol * k;
    p.vol = overwrite || !hand.vol ? p.wr_vol : now.vol * k;
    p.set_fades = overwrite || !hand.fades;
    p.set_length = overwrite || !hand.length;
    p.nudge_s = overwrite ? 0.0 : nudge_s;
    return p;
}

// ---- The Library's view of it ----------------------------------------------------------------

SyncState BuildSyncState(const SoundStore& store, const std::vector<SyncPlacedItem>& items, const std::vector<SyncOwner>& owners)
{
    SyncState                     st;
    std::map<std::string, size_t> by_key;
    std::set<std::string>         owners_behind;
    for (const SyncPlacedItem& it : items) {
        if (it.tag.own != Ownership::Populated) continue;
        if (!it.tag.lib.empty())
            if (const SoundLibrary* l = LibraryOfNode(store, it.tag.node)) st.libs_placed.insert(l->uid);
        // 13-35b: a structure change reshapes it (Populate's); a sound both behind and reshaped is reshaped only.
        const ReshapeVerdict           rs = ReshapeOf(it.tag, store);
        const bool                     reshaped = rs.kind != Reshape::None;
        const std::vector<std::string> behind = reshaped ? std::vector<std::string>{} : BehindNodes(it.tag, store);
        if (!reshaped && behind.empty()) continue;
        const SoundLibrary* lib = reshaped ? FindLibrary(store, rs.lib) : LibraryOfNode(store, it.tag.node);
        if (!lib) continue;  // its library gone: no row to show it on
        if (reshaped) {
            st.libs_placed.insert(lib->uid);
            st.reshaped.insert(rs.mark);
            if (!st.reshape_what.count(rs.mark)) st.reshape_what[rs.mark] = ReshapeText(rs.kind);
        }
        char anchor[64];
        std::snprintf(anchor, sizeof anchor, "%.4f|%.4f", it.tag.t, it.anchor);
        const std::string key = it.owner + "|" + it.tag.rid + "|" + it.tag.node + "|" + it.tag.pt + "|" + anchor;
        auto              hit = by_key.find(key);
        if (hit == by_key.end()) {
            SyncSound s;
            s.owner = it.owner;
            s.lib = lib->uid;
            s.node = it.tag.node;
            s.behind = behind;
            s.name = it.name;
            s.t = it.tag.t;
            const std::vector<NodePrint> now =
                reshaped ? std::vector<NodePrint>{} : PathPrints(store, *lib, it.tag.node, it.tag.va);
            for (size_t i = 0; i < now.size() && i < it.tag.lib.size(); ++i) {
                if (now[i] == it.tag.lib[i]) continue;
                if (ChangeRedraws(it.tag.lib[i], now[i])) {
                    s.redraw = true;
                    st.redraws.insert(now[i].node);
                }
                if (!st.what.count(now[i].node))
                    st.what[now[i].node] = DescribeChange(it.tag.lib[i], now[i], now[i].node == it.tag.node ? it.tag.va : -1);
            }
            hit = by_key.emplace(key, st.sounds.size()).first;
            st.sounds.push_back(s);
        }
        SyncSound& s = st.sounds[hit->second];
        s.items.push_back(it.guid);
        if (reshaped) {
            s.reshaped = true;
            if (std::find(s.reshaped_at.begin(), s.reshaped_at.end(), rs.mark) == s.reshaped_at.end()) s.reshaped_at.push_back(rs.mark);
        }
        for (const std::string& b : behind)
            if (std::find(s.behind.begin(), s.behind.end(), b) == s.behind.end()) s.behind.push_back(b);
        if (it.tag.has_wr) MergeHandEdit(s.hand, HandEdits(it.tag.wr, it.now, it.nudge_s));
        owners_behind.insert(it.owner);
    }
    for (SyncSound& s : st.sounds)
        if (s.reshaped) {  // reshaped only (Sync would place it from a stale shape)
            s.behind.clear();
            s.redraw = false;
        }
    for (const SyncOwner& o : owners)
        if (owners_behind.count(o.guid)) st.owners.push_back(o);
    return st;
}

std::vector<std::string> SyncSoundMarks(const SyncSound& s)
{
    std::vector<std::string> out = s.behind;
    for (const std::string& r : s.reshaped_at)
        if (std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
    return out;
}

bool SyncInScope(const SyncSound& s, const std::string& lib, const std::string& node)
{
    if (s.lib != lib) return false;
    const std::vector<std::string> marks = SyncSoundMarks(s);
    return node.empty() || std::find(marks.begin(), marks.end(), node) != marks.end();
}

std::string SyncMarkWhat(const SyncState& st, const std::string& node)
{
    std::string t;
    const auto  w = st.what.find(node);
    if (w != st.what.end()) Add(t, w->second);
    const auto r = st.reshape_what.find(node);
    if (r != st.reshape_what.end()) Add(t, r->second);
    return t;
}

bool SyncMarkReshaped(const SyncState& st, const std::string& node) { return st.reshaped.count(node) > 0; }

SyncSplit SplitSyncOwners(const SyncState& st, const std::string& lib, const std::string& node,
                          const std::vector<std::string>& owners)
{
    SyncSplit out;
    for (const std::string& o : owners) {
        bool again = false;
        for (const SyncSound& s : st.sounds)
            if (s.owner == o && s.reshaped && SyncInScope(s, lib, node)) again = true;
        (again ? out.populate : out.sync).push_back(o);
    }
    return out;
}

SyncCard SyncCardOf(const SyncState& st, const std::string& lib)
{
    SyncCard              c;
    std::set<std::string> nodes, items;
    for (const SyncSound& s : st.sounds) {
        if (s.lib != lib) continue;
        ++c.sounds;
        if (s.reshaped) ++c.reshaped;
        items.insert(s.owner);
        const std::vector<std::string> marks = SyncSoundMarks(s);
        nodes.insert(marks.begin(), marks.end());
    }
    c.changes = static_cast<int>(nodes.size());
    c.items = static_cast<int>(items.size());
    return c;
}

std::string SyncPlural(int n, const char* word)
{
    return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
}

std::string SyncCardText(const SyncCard& c)
{
    std::string t = SyncPlural(c.changes, "change") + " not yet on " + SyncPlural(c.sounds, "placed sound") + " in " +
                    SyncPlural(c.items, "item") + ".";
    if (c.reshaped > 0)  // 13-35b
        t += " " + std::string(c.reshaped == c.sounds ? (c.reshaped == 1 ? "It follows" : "They follow") : std::to_string(c.reshaped) + (c.reshaped == 1 ? " of them follows" : " of them follow")) +
             " a structure change of the library: Populate again to update " + (c.reshaped == 1 ? "it." : "them.");
    return t;
}

std::map<std::string, int> SyncMarksOf(const SyncState& st, const std::string& lib)
{
    std::map<std::string, int> out;
    for (const SyncSound& s : st.sounds)
        if (s.lib == lib)
            for (const std::string& b : SyncSoundMarks(s)) ++out[b];
    return out;
}

std::vector<SyncItemLine> SyncItemLines(const SyncState& st, const std::string& lib, const std::string& node)
{
    std::vector<SyncItemLine> lines;
    std::vector<std::string>  tracks;  // in the owners' order
    for (const SyncOwner& o : st.owners) {
        SyncItemLine l;
        l.owner = o.guid;
        l.name = o.name;
        l.track = o.track;
        l.selected = o.selected;
        for (const SyncSound& s : st.sounds) {
            if (s.owner != o.guid || !SyncInScope(s, lib, node)) continue;
            ++l.sounds;
            if (s.reshaped) ++l.reshaped;  // 13-35b: placed again by Populate, never asked about
            else if (s.hand.Any()) ++l.hand;
            for (const std::string& b : SyncSoundMarks(s))
                if ((node.empty() || b == node) && std::find(l.nodes.begin(), l.nodes.end(), b) == l.nodes.end()) l.nodes.push_back(b);
        }
        if (l.sounds == 0) continue;
        if (l.reshaped > 0) l.hand = 0;  // 13-35b: the whole item is placed again by Populate, never asked about
        if (std::find(tracks.begin(), tracks.end(), l.track) == tracks.end()) tracks.push_back(l.track);
        lines.push_back(l);
    }
    std::vector<SyncItemLine> out;
    for (const std::string& t : tracks)
        for (const SyncItemLine& l : lines)
            if (l.track == t) out.push_back(l);
    return out;
}

std::vector<const SyncSound*> SyncHandSounds(const SyncState& st, const std::string& lib, const std::string& node,
                                             const std::set<std::string>& owners)
{
    std::vector<const SyncSound*> out;
    for (const SyncOwner& o : st.owners) {
        if (!owners.count(o.guid)) continue;
        for (const SyncSound& s : st.sounds)
            if (s.owner == o.guid && !s.reshaped && s.hand.Any() && SyncInScope(s, lib, node)) out.push_back(&s);
    }
    return out;
}

bool SyncApplyCheckedEnabled(size_t checked, size_t all) { return checked > 0 && checked < all; }

bool SyncRememberEnabled(const std::vector<char>& overwrite)
{
    for (size_t i = 1; i < overwrite.size(); ++i)
        if ((overwrite[i] != 0) != (overwrite[0] != 0)) return false;
    return true;
}

std::string SyncToastText(const SyncCounts& c, bool remembered)
{
    std::string parts = std::to_string(c.adjusted) + " adjusted";
    if (c.redrawn) parts += ", " + std::to_string(c.redrawn) + " redrawn";
    if (c.kept) parts += ", " + SyncPlural(c.kept, "hand edit") + " kept";
    if (c.overwritten) parts += ", " + std::to_string(c.overwritten) + " overwritten";
    if (c.skipped) parts += ", " + std::to_string(c.skipped) + " skipped";
    if (c.repopulated) parts += ", " + SyncPlural(c.repopulated, "item") + " placed again";  // 13-35b
    std::string out = "One undo point \xE2\x80\x9CRAV: Sync placed sounds\xE2\x80\x9D: " + parts;
    if (remembered) out += " \xC2\xB7 remembered in \xE2\x98\xB0 \xE2\x80\xBA Sounds";
    return out;
}

std::string SyncDbText(double db) { return Signed(db, " dB"); }

}  // namespace rav
