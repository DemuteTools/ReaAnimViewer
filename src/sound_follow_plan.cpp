// SPDX-License-Identifier: MIT
//
// See sound_follow_plan.h (spec 13-9).

#include "sound_follow_plan.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>

#include "event_list.h"
#include "rule_record.h"
#include "sound_draw.h"
#include "sound_record.h"
#include "tagging_signal.h"

namespace rav {

namespace {

std::string Num(double v)
{
    char                       buf[40];
    const std::to_chars_result r = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, r.ptr);
}

bool ReadNum(const std::string& s, double* out)
{
    if (s.empty()) return false;
    double                       v = 0.0;
    const char*                  b = s.data();
    const char*                  e = b + s.size();
    const std::from_chars_result r = std::from_chars(b, e, v);
    if (r.ec != std::errc() || r.ptr != e || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

const FollowItemFacts* FactsOf(const std::vector<FollowItemFacts>& items, const std::string& guid)
{
    for (const FollowItemFacts& f : items)
        if (SameFollowGuid(f.item_guid, guid)) return &f;
    return nullptr;
}

std::string LibraryName(const FollowInput& in, const std::string& uid)
{
    for (const auto& p : in.library_names)
        if (p.first == uid) return p.second;
    return uid.empty() ? std::string("no library") : uid;
}

ReportLine DetachLine(const SoundPlacement& p, const std::string& rid, double t)
{
    (void)rid;
    ReportLine l;
    l.kind = ReportKind::Detached;
    l.text = "Attached sound kept, its event is gone (now a plain item)";
    l.node = p.node;
    l.item_guid = p.item_guid;
    l.t = t;
    return l;
}

// A gone event's placements (F5): populated / spread removed, attached detached.
void GoneEvent(const EventSounds& ev, const std::vector<FollowItemFacts>& items, FollowPlan* plan)
{
    for (const SoundPlacement& p : ev.placed) {
        if (p.own == Ownership::Deleted) continue;
        const FollowItemFacts* f = FactsOf(items, p.item_guid);
        if (!f || !f->present) continue;
        FollowAction a;
        a.item_guid = p.item_guid;
        if (p.own == Ownership::Attached) {
            a.kind = FollowKind::Detach;
            plan->report.push_back(DetachLine(p, ev.rid, ev.t));
        } else {
            a.kind = FollowKind::Remove;
        }
        plan->actions.push_back(a);
    }
}

double BlendOf(const FollowInput& in, const std::string& node)
{
    for (const auto& b : in.blend_ms)
        if (b.first == node) return b.second;
    return 0.0;
}

// F3 / F5: a kept placement of a matched event.
void FollowPlacement(SoundPlacement& p, const FollowEvent& ev, const FollowInput& in, FollowPlan* plan)
{
    const std::vector<FollowItemFacts>& items = in.items;
    if (p.own == Ownership::Deleted) return;
    const FollowItemFacts* f = FactsOf(items, p.item_guid);
    if (!f || !f->present) return;  // handled by the caller (F6)

    // Fix 2026-10-12: the pass this sound sits on (13-8 places one per loop pass). Its stored
    // pass; at first sight (or an older line) the pass nearest its sync point, as Populate finds
    // it (NearestPassOfItem). The event as this sound sees it: that pass's project time, or
    // outside the item when the pass is gone.
    std::vector<FollowPass> passes = ev.passes;
    if (passes.empty() && ev.in_item) passes.push_back(FollowPass{0, ev.project_t});
    bool      have_pass = f->has_store && f->store.has_pass;
    long long pass = have_pass ? f->store.pass : 0;
    if (!have_pass && !passes.empty()) {
        const double anchor = f->pos + f->sync_offset - BlendOf(in, p.node) / 1000.0;
        size_t       best = 0;
        for (size_t k = 1; k < passes.size(); ++k)
            if (std::fabs(passes[k].project_t - anchor) < std::fabs(passes[best].project_t - anchor)) best = k;
        pass = passes[best].id;
        have_pass = true;
    }
    FollowEvent e = ev;
    e.in_item = false;
    e.project_t = 0.0;
    if (have_pass)
        for (const FollowPass& fp : passes)
            if (fp.id == pass) {
                e.in_item = true;
                e.project_t = fp.project_t;
            }
    // F6: a track move.
    if (!f->track_guid.empty() && f->track_guid != p.track_guid) {
        p.track_guid = f->track_guid;
        plan->record_changed = true;
    }
    // F2: the tag follows the event's clip time.
    if (f->has_tag && std::fabs(f->tag_t - e.t) > 1e-9) {
        FollowAction a;
        a.kind = FollowKind::Retag;
        a.item_guid = p.item_guid;
        a.tag_t = e.t;
        plan->actions.push_back(a);
    }
    if (p.own == Ownership::Attached) return;  // never moved nor muted
    // First sight while the event is outside the item: its project time is unknown, nothing is
    // stored nor muted until the event is back in the item.
    if (!f->has_store && !e.in_item) return;

    FollowStore st = f->store;
    st.has_pass = have_pass;
    st.pass = pass;
    double      pos = f->pos;
    if (!f->has_store) {
        // First sight: stored, nothing moved.
        st.pos = pos;
        st.pt = e.project_t;
        st.mute = false;
    } else if (std::fabs(f->pos - f->store.pos) > kFollowPosTolS) {
        // The user moved it: their nudge is kept (both moved = no nudge change).
        const double dpt = e.in_item ? e.project_t - f->store.pt : 0.0;
        const double nudge = (f->pos - f->store.pos) - dpt;
        if (std::fabs(nudge) > 1e-9) {
            p.offset_s += nudge;
            plan->record_changed = true;
        }
        st.pos = pos;
        if (e.in_item) st.pt = e.project_t;
    } else if (e.in_item && std::fabs(e.project_t - f->store.pt) > kFollowPtTolS) {
        pos = f->pos + (e.project_t - f->store.pt);
        FollowAction a;
        a.kind = FollowKind::Move;
        a.item_guid = p.item_guid;
        a.project_t = pos;
        plan->actions.push_back(a);
        st.pos = pos;
        st.pt = e.project_t;
    }
    // F5: outside the item = muted by the follow; a mute of the user's own stays theirs.
    if (!e.in_item) {
        if (!f->muted) {
            plan->actions.push_back(FollowAction{FollowKind::Mute, p.item_guid, 0.0, {}, 0.0});
            st.mute = true;
        }
    } else if (f->has_store && f->store.mute) {
        if (f->muted) plan->actions.push_back(FollowAction{FollowKind::Unmute, p.item_guid, 0.0, {}, 0.0});
        st.mute = false;
    }
    const bool same = f->has_store && st.pos == f->store.pos && st.pt == f->store.pt && st.mute == f->store.mute &&
                      // an older line (no pass) is not rewritten for the pass alone
                      (!f->store.has_pass || (st.has_pass && st.pass == f->store.pass));
    if (!same) {
        FollowAction a;
        a.kind = FollowKind::Store;
        a.item_guid = p.item_guid;
        a.store = st;
        plan->actions.push_back(a);
    }
}

}  // namespace

bool SameFollowGuid(const std::string& a, const std::string& b)
{
    if (a.size() != b.size() || a.empty()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 32);
        if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 32);
        if (x != y) return false;
    }
    return true;
}

uint64_t FollowGuidHash(const std::string& guid)
{
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (char c : guid) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return h ? h : 1;
}

std::string FormatFollowStore(const FollowStore& s)
{
    std::string out = "RAVSNDFOLLOW 1 pos=" + Num(s.pos) + " pt=" + Num(s.pt);
    if (s.mute) out += " mute=1";
    if (s.has_pass) out += " pass=" + std::to_string(s.pass);
    return out;
}

bool ParseFollowStore(const std::string& text, FollowStore* out)
{
    std::istringstream in(text);
    std::string        tok;
    if (!(in >> tok) || tok != "RAVSNDFOLLOW") return false;
    if (!(in >> tok)) return false;
    double ver = 0.0;
    if (!ReadNum(tok, &ver) || ver < 1.0) return false;
    FollowStore s;
    bool        has_pos = false, has_pt = false;
    while (in >> tok) {
        const size_t eq = tok.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
        if (k == "pos") has_pos = ReadNum(v, &s.pos);
        else if (k == "pt") has_pt = ReadNum(v, &s.pt);
        else if (k == "mute") s.mute = v == "1";
        else if (k == "pass") {
            long long         id = 0;
            const char*       b = v.data();
            const char*       e = b + v.size();
            const auto        r = std::from_chars(b, e, id);
            s.has_pass = !v.empty() && r.ec == std::errc() && r.ptr == e;
            s.pass = s.has_pass ? id : 0;
        }
    }
    if (!has_pos || !has_pt) return false;
    *out = s;
    return true;
}

std::vector<FollowPass> FollowPassesOf(const ItemClipMap& m, double clip_t)
{
    std::vector<FollowPass> out;
    if (!std::isfinite(clip_t)) return out;
    const double rate = (m.rate > 0.0 && std::isfinite(m.rate)) ? m.rate : 1.0;
    const double offs = std::isfinite(m.start_offs) ? m.start_offs : 0.0;
    for (const ClipPass& p : ClipPasses(m)) {
        // ClipToProjectTimes' test, pass for pass.
        const double t = p.p0 + (clip_t - p.clip0) / rate;
        if (!(t >= p.p0 - 1e-9 && t < p.p1 - 1e-12)) continue;
        FollowPass fp;
        fp.project_t = t;
        if (m.loop && m.clip_len > 0.0) {
            // The pass starts at source time offs + (p0 - item_pos) * rate = id * clip_len + clip0.
            const double ks = offs + (p.p0 - m.item_pos) * rate - p.clip0;
            fp.id = static_cast<long long>(std::llround(ks / m.clip_len));
        }
        out.push_back(fp);
    }
    return out;
}

std::vector<FollowEvent> SoundEventsOf(const std::vector<ShownEvent>& shown, const std::vector<Block>& blocks,
                                       const ItemClipMap& map)
{
    std::vector<FollowEvent> out;
    for (const ShownEvent& e : shown) {
        if (e.kind != ShownKind::Detected && e.kind != ShownKind::User) continue;
        if (e.block < 0 || static_cast<size_t>(e.block) >= blocks.size()) continue;
        const std::string& rid = blocks[static_cast<size_t>(e.block)].rid;
        if (rid.empty()) continue;
        FollowEvent fe;
        fe.rid = rid;
        fe.t = e.t;
        fe.user = e.kind == ShownKind::User;
        fe.passes = FollowPassesOf(map, e.t);
        fe.in_item = !fe.passes.empty();
        fe.project_t = fe.in_item ? fe.passes.front().project_t : 0.0;
        out.push_back(fe);
    }
    return out;
}

std::vector<FollowEvent> CommittedFollowEvents(const ItemRules& rules, const ItemClipMap& map)
{
    std::vector<FollowEvent> out;
    const int                nb = static_cast<int>(rules.blocks.size());
    for (const EventEntry& e : rules.events) {
        if (e.kind != EventKind::Detected && e.kind != EventKind::User) continue;
        if (e.block < 0 || e.block >= nb) continue;
        const Block& b = rules.blocks[static_cast<size_t>(e.block)];
        if (!b.enabled || b.rid.empty()) continue;
        if (e.kind == EventKind::Detected) {
            bool masked = false;
            for (const EventEntry& s : rules.events)
                if (s.kind == EventKind::Suppress && s.block == e.block && std::fabs(s.t - e.t) <= kSuppressWindowS + 1e-12)
                    masked = true;
            if (masked) continue;
        }
        FollowEvent fe;
        fe.rid = b.rid;
        fe.t = e.t;
        fe.user = e.kind == EventKind::User;
        fe.in_item = FirstPassProjectTime(map, e.t, &fe.project_t);
        if (!fe.in_item) fe.project_t = 0.0;
        out.push_back(fe);
    }
    std::stable_sort(out.begin(), out.end(), [](const FollowEvent& a, const FollowEvent& b) { return a.t < b.t; });
    return out;
}

FollowPlan PlanItemFollow(const FollowInput& in)
{
    FollowPlan plan;
    plan.record = in.rec;
    ItemSounds& rec = plan.record;

    // F8: a copy (all its placements are another item's).
    if (in.appeared) {
        bool any = false, own = false;
        for (const EventSounds& ev : rec.events)
            for (const SoundPlacement& p : ev.placed) {
                if (p.own == Ownership::Deleted) continue;
                any = true;
                const FollowItemFacts* f = FactsOf(in.items, p.item_guid);
                if (f && f->has_tag && SameFollowGuid(f->tag_owner, in.owner_guid)) own = true;
            }
        if (any && !own) {
            for (EventSounds& ev : rec.events) ev.placed.clear();
            if (!in.keep_draw) Reshuffle(rec, in.copy_seed);
            plan.record_changed = true;
            plan.populate_copy = true;
            return plan;
        }
    }

    // A placement whose item is tagged for another owner is not this item's (a copy's record as
    // undo restored it, or a copy seen at project open): dropped, never moved nor removed, so two
    // owners never act on the same item.
    if (!in.owner_guid.empty())
        for (EventSounds& ev : rec.events)
            for (size_t i = 0; i < ev.placed.size();) {
                const FollowItemFacts* f = FactsOf(in.items, ev.placed[i].item_guid);
                if (ev.placed[i].own != Ownership::Deleted && f && f->present && f->has_tag &&
                    !SameFollowGuid(f->tag_owner, in.owner_guid)) {
                    ev.placed.erase(ev.placed.begin() + static_cast<long>(i));
                    plan.record_changed = true;
                    continue;
                }
                ++i;
            }

    // F7: an assignment whose rule is gone.
    std::vector<std::string> orphan;
    for (size_t i = 0; i < rec.rules.size();) {
        const RuleSounds& rs = rec.rules[i];
        if (std::find(in.rule_rids.begin(), in.rule_rids.end(), rs.rid) == in.rule_rids.end()) {
            ReportLine l;
            l.kind = ReportKind::Detached;
            l.text = "Assignment removed: rule no longer exists (" + LibraryName(in, rs.library) + ")";
            l.item_guid = in.owner_guid;
            plan.report.push_back(l);
            orphan.push_back(rs.rid);
            rec.rules.erase(rec.rules.begin() + static_cast<long>(i));
            plan.record_changed = true;
        } else {
            ++i;
        }
    }

    // F2: match each record event to an event of its rid.
    const size_t       ne = rec.events.size();
    std::vector<int>   match(ne, -1);
    std::vector<bool>  used(in.events.size(), false);
    struct Pair { double d; size_t r; size_t e; };
    std::vector<Pair> pairs;
    for (size_t r = 0; r < ne; ++r) {
        if (std::find(orphan.begin(), orphan.end(), rec.events[r].rid) != orphan.end()) continue;
        for (size_t e = 0; e < in.events.size(); ++e) {
            if (in.events[e].rid != rec.events[r].rid) continue;
            const double d = std::fabs(in.events[e].t - rec.events[r].t);
            if (d <= kEventMatchSeconds + 1e-12) pairs.push_back({d, r, e});
        }
    }
    std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.d < b.d; });
    for (const Pair& p : pairs) {
        if (match[p.r] >= 0 || used[p.e]) continue;
        match[p.r] = static_cast<int>(p.e);
        used[p.e] = true;
    }
    // A drag: exactly one old and one new User event left for the rid.
    std::vector<std::string> rids;
    for (size_t r = 0; r < ne; ++r)
        if (match[r] < 0 && std::find(rids.begin(), rids.end(), rec.events[r].rid) == rids.end())
            rids.push_back(rec.events[r].rid);
    for (const std::string& rid : rids) {
        if (std::find(orphan.begin(), orphan.end(), rid) != orphan.end()) continue;
        int old_i = -1, n_old = 0, new_i = -1, n_new = 0;
        for (size_t r = 0; r < ne; ++r)
            if (match[r] < 0 && rec.events[r].rid == rid) { old_i = static_cast<int>(r); ++n_old; }
        for (size_t e = 0; e < in.events.size(); ++e)
            if (!used[e] && in.events[e].rid == rid && in.events[e].user) { new_i = static_cast<int>(e); ++n_new; }
        if (n_old == 1 && n_new == 1) {
            match[static_cast<size_t>(old_i)] = new_i;
            used[static_cast<size_t>(new_i)] = true;
        }
    }

    std::vector<EventSounds> kept;
    kept.reserve(ne);
    for (size_t r = 0; r < ne; ++r) {
        EventSounds& ev = rec.events[r];
        if (match[r] < 0) {
            GoneEvent(ev, in.items, &plan);  // F5
            plan.record_changed = true;
            continue;
        }
        const FollowEvent& e = in.events[static_cast<size_t>(match[r])];
        if (ev.t != e.t) {
            ev.t = e.t;
            plan.record_changed = true;
        }
        for (size_t i = 0; i < ev.placed.size();) {
            SoundPlacement&        p = ev.placed[i];
            const FollowItemFacts* f = FactsOf(in.items, p.item_guid);
            if (p.own != Ownership::Deleted && (!f || !f->present)) {
                // F6: deleted by hand.
                plan.record_changed = true;
                if (p.own == Ownership::Attached) {
                    ev.placed.erase(ev.placed.begin() + static_cast<long>(i));
                    continue;
                }
                p.own = Ownership::Deleted;
            } else {
                FollowPlacement(p, e, in, &plan);
            }
            ++i;
        }
        kept.push_back(std::move(ev));
    }
    rec.events = std::move(kept);
    if (!plan.record_changed) plan.record = in.rec;  // untouched copy, byte for byte
    return plan;
}

FollowPlan PlanOwnerGone(const std::string& owner_guid, const std::vector<SoundPlacement>& placed,
                         const std::vector<FollowItemFacts>& items)
{
    FollowPlan plan;
    for (const SoundPlacement& p : placed) {
        if (p.own == Ownership::Deleted) continue;
        const FollowItemFacts* f = FactsOf(items, p.item_guid);
        if (!f || !f->present) continue;
        // Only an item still tagged for this owner (a copy's tags name the original).
        if (!f->has_tag || !SameFollowGuid(f->tag_owner, owner_guid)) continue;
        FollowAction a;
        a.item_guid = p.item_guid;
        if (p.own == Ownership::Attached) {
            a.kind = FollowKind::Detach;
            ReportLine l = DetachLine(p, std::string(), f->tag_t);
            l.text = "Attached sound kept, its animation item is gone (now a plain item)";
            plan.report.push_back(l);
        } else {
            a.kind = FollowKind::Remove;
        }
        plan.actions.push_back(a);
    }
    return plan;
}

FollowPlan PlanFollow(const ItemSounds& rec, const std::vector<FollowEvent>& events,
                      const std::vector<std::string>& present)
{
    FollowInput in;
    in.rec = rec;
    in.events = events;
    for (const RuleSounds& rs : rec.rules) in.rule_rids.push_back(rs.rid);
    for (const EventSounds& ev : rec.events) in.rule_rids.push_back(ev.rid);
    for (const std::string& g : present) {
        FollowItemFacts f;
        f.item_guid = g;
        f.present = true;
        in.items.push_back(f);
    }
    return PlanItemFollow(in);
}

}  // namespace rav
