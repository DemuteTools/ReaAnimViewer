// SPDX-License-Identifier: MIT
//
// See library_sync_select.h (13-77c).

#include "library_sync_select.h"

#include <algorithm>
#include <set>

namespace rav {

namespace {

const char kUp[] = "\xE2\x86\x91";
const char kDown[] = "\xE2\x86\x93";
const char kDot[] = " \xC2\xB7 ";

bool Adopts(const SyncChange& c, SyncDir dir)
{
    return (c.side == SyncSide::Here && dir == SyncDir::Up) || (c.side == SyncSide::File && dir == SyncDir::Down);
}

std::vector<std::string> ClosureFor(const SyncDiff& d, const SyncChange& c, SyncDir dir)
{
    return Adopts(c, dir) ? NeedsClosure(d, c.key) : DependentsClosure(d, c.key);
}

bool Under(const SyncDiff& d, const std::string& node, const std::string& id)
{
    std::string x = node;
    for (int i = 0; i < 1000 && !x.empty(); ++i) {
        if (x == id) return true;
        x = SyncParentOf(d, x);
    }
    return false;
}

std::set<std::string> OnSet(const SyncSelection& s, const SyncDiff& d)
{
    std::set<std::string> out;
    for (const SyncChange& c : d.changes)
        if (s.IsOn(c.key)) out.insert(c.key);
    return out;
}

std::string TextOf(const SyncDiff& d, const std::string& key)
{
    const SyncChange* c = d.Find(key);
    return c ? c->text : key;
}

}  // namespace

// ---- the tree ---------------------------------------------------------------------------------------------------

std::string SyncParentOf(const SyncDiff& d, const std::string& id)
{
    if (id.empty() || id == "grp") return std::string();
    if (id.compare(0, 4, "grp:") == 0) return "grp";
    const auto it = d.parent.find(id);
    return it == d.parent.end() ? std::string() : it->second;
}

std::vector<std::string> SyncChangedNodes(const SyncDiff& d)
{
    std::set<std::string> set;
    for (const SyncChange& c : d.changes)
        for (std::string u = c.node; !u.empty() && set.insert(u).second; u = SyncParentOf(d, u)) {}
    return std::vector<std::string>(set.begin(), set.end());
}

std::vector<std::string> SyncTopNodes(const SyncDiff& d, const std::vector<std::string>& nodes)
{
    const std::set<std::string> set(nodes.begin(), nodes.end());
    std::vector<std::string>    top;
    for (const std::string& u : nodes) {
        const std::string p = SyncParentOf(d, u);
        if (p.empty() || !set.count(p)) top.push_back(u);
    }
    auto rank = [&](const std::string& u) { return u == d.root ? 0 : u == "grp" ? 1 : 2; };
    std::stable_sort(top.begin(), top.end(), [&](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
    return top;
}

std::vector<std::string> SyncKeysUnder(const SyncDiff& d, const std::string& id)
{
    std::vector<std::string> keys;
    for (const SyncChange& c : d.changes)
        if (id == "*" || Under(d, c.node, id)) keys.push_back(c.key);
    return keys;
}

std::vector<std::string> SyncKeysAt(const SyncDiff& d, const std::string& id)
{
    std::vector<std::string> keys;
    for (const SyncChange& c : d.changes)
        if (c.node == id) keys.push_back(c.key);
    return keys;
}

std::string SyncGhostName(const SyncDiff& d, const std::string& id)
{
    if (id == "grp") return "Switch Groups";
    if (id.compare(0, 4, "grp:") == 0) {
        static const char* const kSeps[] = {" added", ": ", " changed", " removed", " now"};
        const std::string        pre = "Switch Group ";
        for (const SyncChange& c : d.changes) {
            if (c.node != id || c.text.compare(0, pre.size(), pre) != 0) continue;
            const std::string rest = c.text.substr(pre.size());
            size_t            cut = std::string::npos;
            for (const char* s : kSeps) cut = std::min(cut, rest.find(s));
            return cut == std::string::npos ? rest : rest.substr(0, cut);
        }
        return std::string();
    }
    for (const SyncChange& c : d.changes)
        if (c.subject == id && c.adds_container && !c.label.empty()) return c.label;
    return std::string();
}

std::string SyncGhostType(const SyncDiff& d, const std::string& id)
{
    for (const SyncChange& c : d.changes)
        if (c.subject == id && c.adds_container) {
            const size_t sp = c.text.find(' ');
            return sp == std::string::npos ? std::string() : c.text.substr(0, sp);
        }
    return std::string();
}

std::string SyncChangesWord(int n)
{
    return std::to_string(n) + (n == 1 ? " change" : " changes");
}

bool SyncDefaultDir(const SyncChange& c, SyncDir* out)
{
    if (c.side == SyncSide::Both) return false;
    *out = c.side == SyncSide::Here ? SyncDir::Up : SyncDir::Down;
    return true;
}

std::vector<std::string> SyncKeysForDir(const SyncDiff& d, const std::vector<std::string>& keys, SyncDir dir)
{
    std::vector<std::string> out;
    for (const std::string& k : keys) {
        const SyncChange* c = d.Find(k);
        if (c && (c->side == SyncSide::Both || c->side == (dir == SyncDir::Up ? SyncSide::Here : SyncSide::File))) out.push_back(k);
    }
    return out;
}

void SyncDirCounts(const SyncDiff& d, const std::vector<std::string>& keys, int* up, int* down)
{
    int u = 0, dn = 0;
    for (const std::string& k : keys) {
        const SyncChange* c = d.Find(k);
        if (!c) continue;
        if (c->side != SyncSide::File) ++u;
        if (c->side != SyncSide::Here) ++dn;
    }
    if (up) *up = u;
    if (down) *down = dn;
}

// ---- the selection ----------------------------------------------------------------------------------------------

bool SyncSelection::IsOn(const std::string& key) const
{
    const auto it = ticks_.find(key);
    return it != ticks_.end() && it->second.on;
}

const SyncTick* SyncSelection::Tick(const std::string& key) const
{
    const auto it = ticks_.find(key);
    return it == ticks_.end() ? nullptr : &it->second;
}

void SyncSelection::Put(const SyncDiff&, const SyncChange& c, SyncDir dir, bool auto_, const std::string& by, bool needs_why)
{
    SyncTick t;
    t.on = true;
    t.has_dir = true;
    t.dir = dir;
    t.side = c.side;
    t.auto_ = auto_;
    t.by = by;
    t.needs_why = needs_why;
    t.val = c.text + "\x1f" + c.here_text + "\x1f" + c.file_text;
    ticks_[c.key] = t;
}

void SyncSelection::Pull(const SyncDiff& d, const SyncChange& c, SyncDir dir, SyncEffect*)
{
    if (c.side == SyncSide::Both) return;
    const bool ad = Adopts(c, dir);
    for (const std::string& k : ClosureFor(d, c, dir)) {
        const SyncChange* x = d.Find(k);
        if (!x || x->side != c.side) continue;
        if (IsOn(k)) {
            SyncTick& t = ticks_[k];
            if (t.dir != dir && t.auto_) {  // turned the other way by this change: it is now this one's (H2)
                t.by = c.key;
                t.needs_why = ad;
            }
            t.dir = dir;
        } else {
            Put(d, *x, dir, true, c.key, ad);
        }
    }
}

std::vector<std::string> SyncSelection::Prune(const SyncDiff& d)
{
    std::vector<std::string> gone;
    bool                     again = true;
    while (again) {
        again = false;
        for (const SyncChange& c : d.changes) {
            const auto it = ticks_.find(c.key);
            if (it == ticks_.end() || !it->second.on) continue;
            const SyncTick& s = it->second;
            // its asker went, or turned the other way (H2): another ticked change that needs it keeps it (the mockup lost it)
            auto asker_ok = [&](const SyncTick& t) {
                const SyncTick* b = Tick(t.by);
                return b && b->on && b->dir == t.dir;
            };
            if (s.auto_ && !asker_ok(s)) {
                const std::vector<std::string> others = NeededBy(d, c.key);
                if (!others.empty()) it->second.by = others[0];
            }
            bool            bad = s.auto_ && !asker_ok(s);
            if (!bad && c.side != SyncSide::Both)
                for (const std::string& k : ClosureFor(d, c, s.dir)) {
                    const SyncChange* x = d.Find(k);
                    if (x && x->side == c.side && !IsOn(k)) {
                        bad = true;
                        break;
                    }
                }
            if (bad) {
                it->second.on = false;
                gone.push_back(c.key);
                again = true;
            }
        }
    }
    return gone;
}

bool SyncSelection::SetDir(const SyncDiff& d, const SyncChange& c, SyncDir dir, SyncEffect* fx)
{
    Put(d, c, dir, false, std::string(), true);
    Pull(d, c, dir, fx);
    return true;
}

bool SyncSelection::TickKey(const SyncDiff& d, const SyncChange& c, SyncEffect* fx)
{
    SyncDir     dir;
    const auto  it = ticks_.find(c.key);
    if (it != ticks_.end() && it->second.has_dir) dir = it->second.dir;
    else if (!SyncDefaultDir(c, &dir)) return false;
    return SetDir(d, c, dir, fx);
}

static void Finish(const SyncDiff& d, const SyncSelection& s, const std::set<std::string>& before,
                   const std::vector<std::string>& clicked, SyncEffect* fx)
{
    const std::set<std::string> after = OnSet(s, d);
    const std::set<std::string> ask(clicked.begin(), clicked.end());
    auto                        asked = [&](const std::string& k) { return ask.count(k) != 0; };
    for (const SyncChange& c : d.changes) {
        if (asked(c.key)) continue;
        const bool b = before.count(c.key) != 0, a = after.count(c.key) != 0;
        if (a && !b) fx->ticked.push_back(c.key);
        if (b && !a) fx->unticked.push_back(c.key);
    }
}

SyncEffect SyncSelection::Toggle(const SyncDiff& d, const std::string& key)
{
    SyncEffect        fx;
    const SyncChange* c = d.Find(key);
    if (!c) return fx;
    const std::set<std::string> before = OnSet(*this, d);
    ++ver_;
    if (IsOn(key)) {
        ticks_[key].on = false;
        Prune(d);
    } else if (!TickKey(d, *c, &fx)) {
        fx.need_side = true;
        return fx;
    }
    Finish(d, *this, before, {key}, &fx);
    return fx;
}

SyncEffect SyncSelection::Choose(const SyncDiff& d, const std::string& key, SyncDir dir)
{
    SyncEffect        fx;
    const SyncChange* c = d.Find(key);
    if (!c) return fx;
    const std::set<std::string> before = OnSet(*this, d);
    ++ver_;
    SetDir(d, *c, dir, &fx);
    Prune(d);  // what the old direction had ticked for you goes (H2)
    Finish(d, *this, before, {key}, &fx);
    return fx;
}

SyncEffect SyncSelection::ToggleGroup(const SyncDiff& d, const std::vector<std::string>& keys)
{
    SyncEffect               fx;
    std::vector<std::string> live;
    for (const std::string& k : keys)
        if (d.Find(k)) live.push_back(k);
    if (live.empty()) return fx;
    const std::set<std::string> before = OnSet(*this, d);
    ++ver_;
    // M1: an unsided `both` change cannot be ticked from here, so it does not keep the box from being "all ticked"
    std::vector<std::string> tickable;
    for (const std::string& k : live) {
        const SyncChange& c = *d.Find(k);
        const SyncTick*   t = Tick(k);
        if (c.side != SyncSide::Both || (t && t->has_dir)) tickable.push_back(k);
    }
    const bool all_on = !tickable.empty() && std::all_of(tickable.begin(), tickable.end(), [&](const std::string& k) { return IsOn(k); });
    if (all_on) {
        for (const std::string& k : tickable) ticks_[k].on = false;
        Prune(d);
    } else {
        for (const std::string& k : live) {
            if (IsOn(k)) continue;
            const SyncChange& c = *d.Find(k);
            const auto        it = ticks_.find(k);
            if (c.side == SyncSide::Both && !(it != ticks_.end() && it->second.has_dir)) ++fx.skipped_both;
            else TickKey(d, c, &fx);
        }
    }
    Finish(d, *this, before, live, &fx);  // the group's own keys are what was asked: only the others are "also"
    return fx;
}

SyncEffect SyncSelection::GroupDir(const SyncDiff& d, const std::vector<std::string>& keys, SyncDir dir)
{
    SyncEffect fx;
    const std::set<std::string> before = OnSet(*this, d);
    const std::vector<std::string> asked = SyncKeysForDir(d, keys, dir);
    ++ver_;
    for (const std::string& k : asked) SetDir(d, *d.Find(k), dir, &fx);
    Prune(d);
    Finish(d, *this, before, asked, &fx);
    return fx;
}

SyncTri SyncSelection::TriState(const SyncDiff& d, const std::vector<std::string>& keys) const
{
    int n = 0, on = 0;
    for (const std::string& k : keys) {
        if (!d.Find(k)) continue;
        ++n;
        if (IsOn(k)) ++on;
    }
    return n == 0 ? SyncTri::Disabled : on == 0 ? SyncTri::None : on == n ? SyncTri::All : SyncTri::Some;
}

SyncCountsTicked SyncSelection::Counts(const SyncDiff& d) const
{
    SyncCountsTicked n;
    for (const SyncChange& c : d.changes) {
        const auto it = ticks_.find(c.key);
        if (it == ticks_.end() || !it->second.on) continue;
        ++n.ticked;
        (it->second.dir == SyncDir::Up ? n.up : n.down)++;
        if (it->second.auto_) ++n.auto_;
    }
    return n;
}

std::vector<std::string> SyncSelection::NeededBy(const SyncDiff& d, const std::string& key) const
{
    std::vector<std::string> out;
    const SyncTick*          me = Tick(key);
    if (!me || !me->on) return out;
    auto asks = [&](const SyncChange& x) {
        if (x.key == key || x.side == SyncSide::Both) return false;
        const SyncTick* t = Tick(x.key);
        if (!t || !t->on || t->dir != me->dir) return false;
        const std::vector<std::string> cl = ClosureFor(d, x, t->dir);
        return std::find(cl.begin(), cl.end(), key) != cl.end();
    };
    if (d.IndexValid()) {  // only the changes related to `key` can ask for it (H1): both closures, in diff order
        std::set<size_t> cand;
        for (const std::string& k : NeedsClosure(d, key))
            if (const SyncChange* x = d.Find(k)) cand.insert(static_cast<size_t>(x - d.changes.data()));
        for (const std::string& k : DependentsClosure(d, key))
            if (const SyncChange* x = d.Find(k)) cand.insert(static_cast<size_t>(x - d.changes.data()));
        for (size_t i : cand)
            if (asks(d.changes[i])) out.push_back(d.changes[i].key);
        return out;
    }
    for (const SyncChange& x : d.changes)
        if (asks(x)) out.push_back(x.key);
    return out;
}

std::map<std::string, std::vector<std::string>> SyncSelection::NeededByAll(const SyncDiff& d) const
{
    std::map<std::string, std::vector<std::string>> out;
    for (const SyncChange& c : d.changes) {
        const SyncTick* t = Tick(c.key);
        if (!t || !t->on) continue;
        std::vector<std::string> nb = NeededBy(d, c.key);
        if (!nb.empty()) out[c.key] = std::move(nb);
    }
    return out;
}

std::map<std::string, SyncDir> SyncSelection::Picks(const SyncDiff& d) const
{
    std::map<std::string, SyncDir> out;
    for (const SyncChange& c : d.changes) {
        const auto it = ticks_.find(c.key);
        if (it != ticks_.end() && it->second.on && it->second.has_dir) out[c.key] = it->second.dir;
    }
    return out;
}

void SyncSelection::Rekey(const SyncDiff& now)
{
    for (auto it = ticks_.begin(); it != ticks_.end();) {
        const SyncChange* c = now.Find(it->first);
        const std::string val = c ? c->text + "\x1f" + c->here_text + "\x1f" + c->file_text : std::string();
        if (!c || c->side != it->second.side || val != it->second.val) it = ticks_.erase(it);  // L3: a moved value is not what was ticked
        else ++it;
    }
    ++ver_;
    Prune(now);
}

// ---- texts ------------------------------------------------------------------------------------------------------

std::string SyncEffectToast(const SyncDiff& d, const SyncEffect& fx)
{
    if (fx.need_side) return std::string("Changed on both sides: pick ") + kUp + " Send mine or " + kDown + " Get file's";
    auto join = [&](const std::vector<std::string>& keys) {
        std::string s;
        for (size_t i = 0; i < keys.size(); ++i) s += (i ? ", " : "") + TextOf(d, keys[i]);
        return s;
    };
    std::string out;
    if (!fx.ticked.empty()) out = "Also ticked: " + join(fx.ticked);
    if (!fx.unticked.empty()) out += std::string(out.empty() ? "" : "\n") + "Also unticked, they went together: " + join(fx.unticked);
    if (fx.skipped_both > 0)
        out += std::string(out.empty() ? "" : "\n") + std::to_string(fx.skipped_both) + " changed on both sides: pick a side for each";
    return out;
}

std::string SyncFooterText(const SyncCountsTicked& c, int left)
{
    if (left <= 0) return "Nothing left: this project and the file agree.";
    if (c.ticked <= 0) return "Nothing ticked. " + SyncChangesWord(left) + " waiting.";
    std::string s = std::to_string(c.ticked) + " ticked: " + kUp + " " + std::to_string(c.up) + " to the file, " + kDown + " " +
                    std::to_string(c.down) + " taken here";
    if (c.auto_ > 0) s += std::string(kDot) + std::to_string(c.auto_) + " ticked for you because another needs it";
    return s;
}

}  // namespace rav
