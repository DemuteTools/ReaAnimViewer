// SPDX-License-Identifier: MIT
//
// See sound_follow_plan.h (spec 13-9).

#include "sound_follow_plan.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
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

FollowAction Act(FollowKind k, const std::string& guid)
{
    FollowAction a;
    a.kind = k;
    a.item_guid = guid;
    return a;
}

// Fix C 2026-10-12: one mute step. `why` = why the follow wants the sound silent (kMuteOutside,
// kMuteDisabled; 0 = it does not). A sound the follow muted and the user unmuted is left alone
// until that reason ends; then the next reason (or another one starting) may mute it again.
void MuteStep(int why, const FollowItemFacts& f, FollowStore& st, FollowPlan* plan)
{
    if (why) {
        if (st.hand == why) return;
        st.hand = 0;  // given for another reason: this one decides again
        if (st.mute) {
            if (!f.muted) {  // the follow muted it, the user unmuted it: theirs now
                st.mute = false;
                st.hand = why;
            }
            return;
        }
        if (!f.muted) {
            plan->actions.push_back(Act(FollowKind::Mute, f.item_guid));
            st.mute = true;
        }
        return;
    }
    if (st.mute && f.muted) plan->actions.push_back(Act(FollowKind::Unmute, f.item_guid));
    st.mute = false;
    st.hand = 0;
}

bool SameStore(const FollowStore& a, const FollowStore& b)
{
    return a.pos == b.pos && a.pt == b.pt && a.mute == b.mute && a.hand == b.hand &&
           // an older line (no pass) is not rewritten for the pass alone
           (!b.has_pass || (a.has_pass && a.pass == b.pass));
}

void StoreIfChanged(const FollowItemFacts& f, const FollowStore& st, FollowPlan* plan)
{
    if (f.has_store && SameStore(st, f.store)) return;
    FollowAction a = Act(FollowKind::Store, f.item_guid);
    a.store = st;
    plan->actions.push_back(a);
}

// Fix C 2026-10-12: a disabled rule's placement whose event is not in the list now (its Detected
// events are not detected while it is disabled): kept where it is, muted.
void HoldPlacement(const SoundPlacement& p, const FollowItemFacts& f, FollowPlan* plan)
{
    if (p.own == Ownership::Deleted) return;
    // A populated sound never stored has no known project time: left until it is followed again.
    if (!f.has_store && p.own != Ownership::Attached) return;
    FollowStore st = f.store;
    if (!f.has_store) {
        st.pos = f.pos;
        st.pt = f.pos;
    }
    MuteStep(kMuteDisabled, f, st, plan);
    StoreIfChanged(f, st, plan);
}

bool Has(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

bool HasGuid(const std::vector<std::string>& v, const std::string& g)
{
    for (const std::string& x : v)
        if (SameFollowGuid(x, g)) return true;
    return false;
}

// The same GUIDs on both sides (any case, any order, repeats ignored).
bool SameGuidSet(const std::vector<std::string>& a, const std::vector<std::string>& b)
{
    for (const std::string& x : a)
        if (!HasGuid(b, x)) return false;
    for (const std::string& x : b)
        if (!HasGuid(a, x)) return false;
    return true;
}

// Fix C 2026-10-12: the copied sound item of `src` (the placement's item before the copy / move):
// tagged for `src_owner`, the same rule, event and node, at src's position + delta. -1 = none.
int FindStray(const std::vector<FollowItemFacts>& strays, const std::vector<bool>& claimed, const FollowItemFacts& src,
              const std::string& src_owner, const EventSounds& ev, const SoundPlacement& p, double delta)
{
    int    best = -1;
    double best_d = 0.0;
    for (size_t i = 0; i < strays.size(); ++i) {
        const FollowItemFacts& s = strays[i];
        if (claimed[i] || !s.present || !s.has_tag || !SameFollowGuid(s.tag_owner, src_owner)) continue;
        if (!s.tag_rid.empty() && s.tag_rid != ev.rid) continue;
        if (!s.tag_node.empty() && !p.node.empty() && s.tag_node != p.node) continue;
        if (std::fabs(s.tag_t - ev.t) > kEventMatchSeconds + 1e-12) continue;
        const double d = std::fabs(s.pos - (src.pos + delta));
        if (d > kStrayPosTolS) continue;
        if (best < 0 || d < best_d) {
            best = static_cast<int>(i);
            best_d = d;
        }
    }
    return best;
}

// Fix C review 2: an owner in the project now (the copy itself, or an animation item).
bool OwnerPresent(const FollowInput& in, const std::string& g)
{
    if (SameFollowGuid(g, in.owner_guid)) return true;
    for (const auto& o : in.owner_positions)
        if (SameFollowGuid(o.first, g)) return true;
    return false;
}

double BlendOf(const FollowInput& in, const std::string& node);

// Fix C review 2: the sound item pasted with a copy whose source is no longer in the project (a
// second paste after a cut, a copy pasted after its source was deleted): tagged for a gone owner,
// the placement's rule, node and event. By the gone owner's remembered facts when the follow
// still has them (its position + the copy's offset), else by where Populate places it on the copy
// (a populated / spread sound: its pile anchor + sync - Blend - nudge on a pass of the event; an
// attached one: inside the copy's span, the only such item). -1 = none.
int FindGoneSourceStray(const FollowInput& in, const std::vector<bool>& claimed, const EventSounds& ev,
                        const SoundPlacement& p)
{
    for (const FollowOwnerMemory& m : in.remembered) {
        if (m.owner.empty() || OwnerPresent(in, m.owner)) continue;
        const FollowItemFacts* src = FactsOf(m.facts, p.item_guid);
        if (!src) continue;
        const int k = FindStray(in.strays, claimed, *src, m.owner, ev, p, in.owner_pos - m.pos);
        if (k >= 0) return k;
    }
    // The copy's event now (its passes inside the copy).
    const FollowEvent* e = nullptr;
    for (const FollowEvent& x : in.events) {
        if (x.rid != ev.rid || std::fabs(x.t - ev.t) > kEventMatchSeconds + 1e-12) continue;
        if (!e || std::fabs(x.t - ev.t) < std::fabs(e->t - ev.t)) e = &x;
    }
    std::vector<FollowPass> passes;
    if (e) {
        passes = e->passes;
        if (passes.empty() && e->in_item) passes.push_back(FollowPass{0, e->project_t});
    }
    const bool attached = p.own == Ownership::Attached;
    int        best = -1, n = 0;
    double     best_d = 0.0;
    for (size_t i = 0; i < in.strays.size(); ++i) {
        const FollowItemFacts& s = in.strays[i];
        if (claimed[i] || !s.present || !s.has_tag || s.tag_owner.empty() || OwnerPresent(in, s.tag_owner)) continue;
        if (s.tag_rid.empty() || s.tag_rid != ev.rid) continue;
        if (!s.tag_node.empty() && !p.node.empty() && s.tag_node != p.node) continue;
        if (std::fabs(s.tag_t - ev.t) > kEventMatchSeconds + 1e-12) continue;
        double d = 0.0;
        if (attached) {
            if (!(in.owner_len > 0.0) || s.pos < in.owner_pos - kStrayPosTolS ||
                s.pos > in.owner_pos + in.owner_len + kStrayPosTolS)
                continue;
        } else {
            if (passes.empty()) continue;
            const double apos = s.tag_has_anchor ? s.pos - s.tag_ao : s.pos;
            const double sync = s.tag_has_anchor ? s.tag_sy : s.sync_offset;
            const double at = apos + sync - BlendOf(in, p.node) / 1000.0 - p.offset_s;
            d = -1.0;
            for (const FollowPass& fp : passes)
                if (d < 0.0 || std::fabs(fp.project_t - at) < d) d = std::fabs(fp.project_t - at);
            if (d > kStrayPosTolS) continue;
        }
        ++n;
        if (best < 0 || d < best_d) {
            best = static_cast<int>(i);
            best_d = d;
        }
    }
    if (attached && n != 1) return -1;  // nothing ties an attached sound to its place: only when alone
    return best;
}

// Fix C review 2: names compared any case (ASCII, and the accented capitals of UTF-8 Latin-1).
std::string FoldName(const char* text)
{
    std::string out = text ? text : "";
    for (size_t i = 0; i < out.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(out[i]);
        if (c >= 'A' && c <= 'Z') {
            out[i] = static_cast<char>(c + 32);
        } else if (c == 0xC3 && i + 1 < out.size()) {
            const unsigned char d = static_cast<unsigned char>(out[i + 1]);
            if (d >= 0x80 && d <= 0x9E && d != 0x97) out[i + 1] = static_cast<char>(d + 0x20);
            ++i;
        }
    }
    return out;
}

bool WordByte(char c)
{
    const unsigned char u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u >= 0x80;
}

// `needle` (folded) in `hay` (folded); `word_start`: only where a word starts.
bool HasFolded(const std::string& hay, const std::string& needle, bool word_start)
{
    if (needle.empty()) return false;
    for (size_t i = hay.find(needle); i != std::string::npos; i = hay.find(needle, i + 1))
        if (!word_start || i == 0 || !WordByte(hay[i - 1])) return true;
    return false;
}

bool HasAnyLocalized(const std::string& hay, const std::vector<std::string>& localized)
{
    for (const std::string& l : localized)
        if (HasFolded(hay, FoldName(l.c_str()), false)) return true;
    return false;
}

FollowAction RetagOwner(const std::string& guid, double t, const std::string& owner)
{
    FollowAction a = Act(FollowKind::Retag, guid);
    a.tag_t = t;
    a.tag_owner = owner;
    return a;
}

double BlendOf(const FollowInput& in, const std::string& node)
{
    for (const auto& b : in.blend_ms)
        if (b.first == node) return b.second;
    return 0.0;
}

// F3 / F5: a kept placement of a matched event.
void FollowPlacement(SoundPlacement& p, const FollowEvent& ev, const FollowInput& in, bool disabled, FollowPlan* plan)
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
        // The pile's anchor (13-8 tag ao / sy), as Populate finds it: one pile, one pass.
        const double apos = f->tag_has_anchor ? f->pos - f->tag_ao : f->pos;
        const double sync = f->tag_has_anchor ? f->tag_sy : f->sync_offset;
        const double anchor = apos + sync - BlendOf(in, p.node) / 1000.0;
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
    if (p.own == Ownership::Attached) {
        // Never moved, nor muted for an event outside the item; muted while its rule is disabled
        // (fix C 2026-10-12), unmuted when it is enabled again.
        if (!disabled && !(f->has_store && (f->store.mute || f->store.hand))) return;
        FollowStore st = f->store;
        if (!f->has_store) {
            st.pos = f->pos;
            st.pt = f->pos;
        }
        MuteStep(disabled ? kMuteDisabled : 0, *f, st, plan);
        StoreIfChanged(*f, st, plan);
        return;
    }
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
        st.hand = 0;
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
    // F5: outside the item (or its rule disabled) = muted by the follow; a mute of the user's own
    // stays theirs, and so does an unmute of a sound the follow muted (fix C 2026-10-12).
    MuteStep(disabled ? kMuteDisabled : (!e.in_item ? kMuteOutside : 0), *f, st, plan);
    StoreIfChanged(*f, st, plan);
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
    if (s.hand) out += " hand=" + std::to_string(s.hand);
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
        else if (k == "hand") s.hand = v == "2" ? kMuteDisabled : (v == "1" ? kMuteOutside : 0);
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
        fe.enabled = blocks[static_cast<size_t>(e.block)].enabled;
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

std::vector<std::pair<size_t, size_t>> MatchMovedOwners(const std::vector<MoveCandidate>& gone,
                                                        const std::vector<MoveCandidate>& appeared)
{
    auto same = [](const MoveCandidate& g, const MoveCandidate& a) {
        if (g.rules_raw.empty() || g.rules_raw != a.rules_raw || g.path != a.path) return false;
        if (!SameGuidSet(g.placed, a.placed)) return false;
        for (const std::string& o : a.owners)
            if (!HasGuid(g.owners, o) && !SameFollowGuid(o, g.guid)) return false;
        return true;
    };
    std::vector<std::pair<size_t, size_t>> out;
    for (size_t g = 0; g < gone.size(); ++g) {
        int j = -1, n = 0;
        for (size_t a = 0; a < appeared.size(); ++a)
            if (same(gone[g], appeared[a])) {
                j = static_cast<int>(a);
                ++n;
            }
        if (n != 1) continue;
        int m = 0;
        for (size_t h = 0; h < gone.size(); ++h) m += same(gone[h], appeared[static_cast<size_t>(j)]);
        if (m == 1) out.emplace_back(g, static_cast<size_t>(j));
    }
    return out;
}

FollowPlan PlanItemFollow(const FollowInput& in0)
{
    FollowInput in = in0;  // its items grow with the sound items a move adopts
    FollowPlan plan;
    plan.record = in.rec;
    ItemSounds& rec = plan.record;

    // Fix C 2026-10-12: a move (cut and paste, a new GUID). Its placed items still tagged for the
    // GUID it had are retagged; the ones cut and pasted with it (new GUIDs, in no record) are found
    // by their tag and position and adopted. The record is kept as it is: no detach, no new draw.
    if (!in.moved_from.empty()) {
        std::vector<bool> claimed(in.strays.size(), false);
        for (EventSounds& ev : rec.events)
            for (SoundPlacement& p : ev.placed) {
                if (p.own == Ownership::Deleted) continue;
                FollowItemFacts* f = nullptr;
                for (FollowItemFacts& x : in.items)
                    if (SameFollowGuid(x.item_guid, p.item_guid)) f = &x;
                if (f && f->present) {
                    if (f->has_tag && (SameFollowGuid(f->tag_owner, in.moved_from) || HasGuid(in.moved_also, f->tag_owner))) {
                        plan.actions.push_back(RetagOwner(p.item_guid, f->tag_t, in.owner_guid));
                        f->tag_owner = in.owner_guid;
                    }
                    continue;
                }
                const FollowItemFacts* src = FactsOf(in.moved_items, p.item_guid);
                if (!src) continue;
                const int k = FindStray(in.strays, claimed, *src, in.moved_from, ev, p, in.moved_delta);
                if (k < 0) continue;
                claimed[static_cast<size_t>(k)] = true;
                FollowItemFacts sf = in.strays[static_cast<size_t>(k)];
                plan.actions.push_back(RetagOwner(sf.item_guid, sf.tag_t, in.owner_guid));
                sf.tag_owner = in.owner_guid;
                p.item_guid = sf.item_guid;
                in.items.push_back(sf);
                plan.record_changed = true;
            }
    }

    // F8: a copy (all its placements are another item's).
    if (in.appeared && in.moved_from.empty()) {
        bool        any = false, own = false;
        std::string source;
        for (const EventSounds& ev : rec.events)
            for (const SoundPlacement& p : ev.placed) {
                if (p.own == Ownership::Deleted) continue;
                any = true;
                const FollowItemFacts* f = FactsOf(in.items, p.item_guid);
                if (f && f->has_tag && SameFollowGuid(f->tag_owner, in.owner_guid)) own = true;
                if (f && f->present && f->has_tag && source.empty()) source = f->tag_owner;
            }
        if (any && !own) {
            // Fix C 2026-10-12: the sound items copied with it (tagged for the source, in no
            // record, at the same place relative to it). Keep the draw on: adopted as its own.
            // Off: the populated ones removed (a new draw is placed), attached and spread adopted.
            bool   has_delta = false;
            double delta = 0.0;
            for (const auto& o : in.owner_positions)
                if (!source.empty() && SameFollowGuid(o.first, source)) {
                    has_delta = true;
                    delta = in.owner_pos - o.second;
                }
            std::vector<bool> claimed(in.strays.size(), false);
            bool              missing = false;
            for (EventSounds& ev : rec.events) {
                std::vector<SoundPlacement> kept;
                for (const SoundPlacement& p : ev.placed) {
                    if (p.own == Ownership::Deleted) {
                        if (in.keep_draw) kept.push_back(p);
                        continue;
                    }
                    const FollowItemFacts* f = FactsOf(in.items, p.item_guid);
                    int k = (has_delta && f && f->present && f->has_tag)
                                ? FindStray(in.strays, claimed, *f, f->tag_owner, ev, p, delta)
                                : -1;
                    // Fix C review 2: its source item is gone (and so may be the source owner):
                    // the sound item pasted with it, tagged for a gone owner.
                    if (k < 0 && !(f && f->present)) k = FindGoneSourceStray(in, claimed, ev, p);
                    if (k < 0) {
                        missing = true;
                        continue;
                    }
                    claimed[static_cast<size_t>(k)] = true;
                    const FollowItemFacts& sf = in.strays[static_cast<size_t>(k)];
                    if (p.own == Ownership::Populated && !in.keep_draw) {
                        plan.actions.push_back(Act(FollowKind::Remove, sf.item_guid));
                        missing = true;
                        continue;
                    }
                    SoundPlacement q = p;
                    q.item_guid = sf.item_guid;
                    if (!sf.track_guid.empty()) q.track_guid = sf.track_guid;
                    plan.actions.push_back(RetagOwner(sf.item_guid, ev.t, in.owner_guid));
                    kept.push_back(q);
                }
                ev.placed = std::move(kept);
            }
            if (!in.keep_draw) Reshuffle(rec, in.copy_seed);
            plan.record_changed = true;
            plan.populate_copy = missing || !in.keep_draw;
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
        // Fix C 2026-10-12: a disabled rule's events are paused, not gone: kept, muted.
        const bool disabled = Has(in.disabled_rids, ev.rid) && !Has(orphan, ev.rid);
        if (match[r] < 0 && disabled) {
            for (size_t i = 0; i < ev.placed.size();) {
                SoundPlacement&        p = ev.placed[i];
                const FollowItemFacts* f = FactsOf(in.items, p.item_guid);
                if (p.own != Ownership::Deleted && (!f || !f->present)) {
                    plan.record_changed = true;  // F6: deleted by hand
                    if (p.own == Ownership::Attached) {
                        ev.placed.erase(ev.placed.begin() + static_cast<long>(i));
                        continue;
                    }
                    p.own = Ownership::Deleted;
                } else if (f) {
                    HoldPlacement(p, *f, &plan);
                }
                ++i;
            }
            kept.push_back(std::move(ev));
            continue;
        }
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
                FollowPlacement(p, e, in, disabled, &plan);
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
                         const std::vector<FollowItemFacts>& items, const std::vector<std::string>& also_owners)
{
    FollowPlan plan;
    for (const SoundPlacement& p : placed) {
        if (p.own == Ownership::Deleted) continue;
        const FollowItemFacts* f = FactsOf(items, p.item_guid);
        if (!f || !f->present) continue;
        // Only an item still tagged for this owner (a copy's tags name the original).
        if (!f->has_tag || (!SameFollowGuid(f->tag_owner, owner_guid) && !HasGuid(also_owners, f->tag_owner))) continue;
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

void FactsAfterPlan(const FollowPlan& plan, std::vector<FollowItemFacts>* facts)
{
    if (!facts) return;
    for (const FollowAction& a : plan.actions)
        for (FollowItemFacts& f : *facts) {
            if (!SameFollowGuid(f.item_guid, a.item_guid)) continue;
            switch (a.kind) {
            case FollowKind::Move: f.pos = a.project_t; break;
            case FollowKind::Remove: f.present = false; break;
            case FollowKind::Detach: f.has_tag = false; break;
            case FollowKind::Mute: f.muted = true; break;
            case FollowKind::Unmute: f.muted = false; break;
            case FollowKind::Store:
                f.has_store = true;
                f.store = a.store;
                break;
            case FollowKind::Retag:
                f.tag_t = a.tag_t;
                if (!a.tag_owner.empty()) f.tag_owner = a.tag_owner;
                break;
            case FollowKind::Forget: break;
            }
        }
}

bool IsCutUndoName(const char* desc, const std::vector<std::string>& localized)
{
    if (!desc) return false;
    const std::string folded = FoldName(desc);
    if (HasAnyLocalized(folded, localized) || HasFolded(folded, "couper", true)) return true;
    auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    const size_t n = std::strlen(desc);
    for (size_t i = 0; i + 3 <= n; ++i) {
        const char c0 = desc[i], c1 = desc[i + 1], c2 = desc[i + 2];
        if ((c0 != 'c' && c0 != 'C') || (c1 != 'u' && c1 != 'U') || (c2 != 't' && c2 != 'T')) continue;
        if (i > 0 && letter(desc[i - 1])) continue;          // "exeCUTe"
        if (i + 3 < n && letter(desc[i + 3])) continue;      // "cutoff"
        return true;
    }
    return false;
}

bool IsSelectionUndoName(const char* desc, const std::vector<std::string>& localized)
{
    if (!desc) return false;
    const std::string folded = FoldName(desc);
    return HasFolded(folded, "select", false) || HasFolded(folded, "s\xC3\xA9lection", false) ||
           HasAnyLocalized(folded, localized);
}

std::vector<bool> UpdateSeenOwners(std::set<std::string>* seen, const std::vector<std::string>& keys, bool scanned)
{
    std::vector<bool> out;
    out.reserve(keys.size());
    std::set<std::string> now;
    for (const std::string& k : keys) {
        out.push_back(scanned && seen && seen->count(k) == 0);
        now.insert(k);
    }
    if (seen) *seen = std::move(now);
    return out;
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
