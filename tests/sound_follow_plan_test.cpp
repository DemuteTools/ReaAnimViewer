// SPDX-License-Identifier: MIT
//
// Host test of src/sound_follow_plan.h (story 13-9): every row of the spec's I/O matrix, the
// follow store's line, CommittedFollowEvents.

#include "sound_follow_plan.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "event_list.h"
#include "rule_record.h"
#include "tagging_signal.h"

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++g_fail;                                                         \
        }                                                                     \
    } while (0)

static bool Near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

static const char* kOwner = "{OWNER}";

static FollowItemFacts Fact(const char* guid, double pos, bool store, double spos, double spt, double tag_t = 1.0)
{
    FollowItemFacts f;
    f.item_guid = guid;
    f.present = true;
    f.pos = pos;
    f.track_guid = "{T1}";
    f.has_store = store;
    f.store.pos = spos;
    f.store.pt = spt;
    f.has_tag = true;
    f.tag_owner = kOwner;
    f.tag_t = tag_t;
    return f;
}

static FollowEvent Ev(const char* rid, double t, double pt, bool user = false, bool in = true)
{
    FollowEvent e;
    e.rid = rid;
    e.t = t;
    e.project_t = pt;
    e.user = user;
    e.in_item = in;
    return e;
}

static FollowInput OneSound(Ownership own, double t = 1.0)
{
    FollowInput in;
    in.owner_guid = kOwner;
    RuleSounds rs;
    rs.rid = "r1";
    rs.library = "L1";
    in.rec.rules.push_back(rs);
    EventSounds ev;
    ev.rid = "r1";
    ev.t = t;
    SoundDraw d;
    d.node = "n1";
    d.pick = 2;
    ev.draws.push_back(d);
    SoundPlacement p;
    p.item_guid = "{A}";
    p.node = "n1";
    p.own = own;
    p.track_guid = "{T1}";
    ev.placed.push_back(p);
    in.rec.events.push_back(ev);
    in.rule_rids = {"r1"};
    in.library_names = {{"L1", "Footsteps"}};
    return in;
}

static int Count(const FollowPlan& p, FollowKind k)
{
    int n = 0;
    for (const auto& a : p.actions) n += a.kind == k;
    return n;
}
static const FollowAction* Find(const FollowPlan& p, FollowKind k)
{
    for (const auto& a : p.actions)
        if (a.kind == k) return &a;
    return nullptr;
}

// Applies the plan to the facts as the REAPER side would, then plans again: nothing.
static void CheckIdempotent(FollowInput in, const FollowPlan& p)
{
    in.rec = p.record;
    for (const FollowAction& a : p.actions) {
        for (FollowItemFacts& f : in.items) {
            if (!SameFollowGuid(f.item_guid, a.item_guid)) continue;
            if (a.kind == FollowKind::Move) f.pos = a.project_t;
            if (a.kind == FollowKind::Store) { f.has_store = true; f.store = a.store; }
            if (a.kind == FollowKind::Mute) f.muted = true;
            if (a.kind == FollowKind::Unmute) f.muted = false;
            if (a.kind == FollowKind::Retag) f.tag_t = a.tag_t;
            if (a.kind == FollowKind::Remove) f.present = false;
            if (a.kind == FollowKind::Detach) f.has_tag = false;
        }
    }
    in.appeared = false;
    const FollowPlan q = PlanItemFollow(in);
    CHECK(q.actions.empty());
    CHECK(!q.record_changed);
}

int main()
{
    // Store line round trip.
    {
        FollowStore s{9.9, 10.0 / 3.0, true}, r;
        CHECK(ParseFollowStore(FormatFollowStore(s), &r));
        CHECK(r.pos == s.pos && r.pt == s.pt && r.mute);
        FollowStore s2{1.5, 2.0, false};
        CHECK(FormatFollowStore(s2) == "RAVSNDFOLLOW 1 pos=1.5 pt=2");
        CHECK(ParseFollowStore("RAVSNDFOLLOW 2 pt=2 pos=1.5 extra=x", &r) && r.pos == 1.5 && !r.mute);
        CHECK(!ParseFollowStore("RAVSNDFOLLOW 1 pos=1", &r));
        CHECK(!ParseFollowStore("OTHER 1 pos=1 pt=2", &r));
        CHECK(!ParseFollowStore("", &r));
    }
    // Item moved: store (9.9, 10), event now 11, pos 9.9 -> Move 10.9, store (10.9, 11).
    {
        FollowInput in = OneSound(Ownership::Populated);
        in.events = {Ev("r1", 1.0, 11.0)};
        in.items = {Fact("{A}", 9.9, true, 9.9, 10.0)};
        const FollowPlan p = PlanItemFollow(in);
        const FollowAction* m = Find(p, FollowKind::Move);
        CHECK(m && Near(m->project_t, 10.9));
        const FollowAction* s = Find(p, FollowKind::Store);
        CHECK(s && Near(s->store.pos, 10.9) && Near(s->store.pt, 11.0));
        CHECK(!p.record_changed);
        CheckIdempotent(in, p);
    }
    // Nudge: store (9.9, 10); pos 10.0, event 10 -> no move, offset +0.1.
    {
        FollowInput in = OneSound(Ownership::Populated);
        in.events = {Ev("r1", 1.0, 10.0)};
        in.items = {Fact("{A}", 10.0, true, 9.9, 10.0)};
        const FollowPlan p = PlanItemFollow(in);
        CHECK(Count(p, FollowKind::Move) == 0);
        CHECK(p.record_changed && Near(p.record.events[0].placed[0].offset_s, 0.1, 1e-9));
        const FollowAction* s = Find(p, FollowKind::Store);
        CHECK(s && Near(s->store.pos, 10.0) && Near(s->store.pt, 10.0));
        CheckIdempotent(in, p);
    }
    // Both moved: store (9.9, 10); pos 10.9, event 11 -> no move, offset unchanged.
    {
        FollowInput in = OneSound(Ownership::Populated);
        in.events = {Ev("r1", 1.0, 11.0)};
        in.items = {Fact("{A}", 10.9, true, 9.9, 10.0)};
        const FollowPlan p = PlanItemFollow(in);
        CHECK(Count(p, FollowKind::Move) == 0);
        CHECK(!p.record_changed);
        CHECK(Near(p.record.events[0].placed[0].offset_s, 0.0));
        CheckIdempotent(in, p);
    }
    // Re-detect within 30 ms: kept, t updated, moved by dpt, draw kept, tag t updated.
    {
        FollowInput in = OneSound(Ownership::Populated);
        in.events = {Ev("r1", 1.020, 10.02)};
        in.items = {Fact("{A}", 9.9, true, 9.9, 10.0)};
        const FollowPlan p = PlanItemFollow(in);
        CHECK(p.record_changed && Near(p.record.events[0].t, 1.020));
        CHECK(p.record.events[0].draws.size() == 1 && p.record.events[0].draws[0].pick == 2);
        const FollowAction* m = Find(p, FollowKind::Move);
        CHECK(m && Near(m->project_t, 9.92));
        const FollowAction* r = Find(p, FollowKind::Retag);
        CHECK(r && Near(r->tag_t, 1.020));
        CheckIdempotent(in, p);
    }
    // Drag: old 1.0 unmatched, one new User 2.0 -> moved, t 2.0.
    {
        FollowInput in = OneSound(Ownership::Populated);
        in.events = {Ev("r1", 2.0, 11.0, true)};
        in.items = {Fact("{A}", 9.9, true, 9.9, 10.0)};
        const FollowPlan p = PlanItemFollow(in);
        CHECK(p.record.events.size() == 1 && Near(p.record.events[0].t, 2.0));
        const FollowAction* m = Find(p, FollowKind::Move);
        CHECK(m && Near(m->project_t, 10.9));
        CHECK(Count(p, FollowKind::Remove) == 0);
        CheckIdempotent(in, p);
    }
    // Re-detect elsewhere: new Detected 3.0 -> populated removed, no pairing.
    {
        FollowInput in = OneSound(Ownership::Populated);
        in.events = {Ev("r1", 3.0, 12.0, false)};
        in.items = {Fact("{A}", 9.9, true, 9.9, 10.0)};
        const FollowPlan p = PlanItemFollow(in);
        CHECK(Count(p, FollowKind::Remove) == 1 && Count(p, FollowKind::Move) == 0);
        CHECK(p.record.events.empty() && p.record_changed);
        CheckIdempotent(in, p);
    }
    // Two old, two new User: no drag pairing.
    {
        FollowInput in = OneSound(Ownership::Populated);
        EventSounds e2 = in.rec.events[0];
        e2.t = 5.0;
        e2.placed[0].item_guid = "{B}";
        in.rec.events.push_back(e2);
        in.events = {Ev("r1", 2.0, 11.0, true), Ev("r1", 7.0, 16.0, true)};
        in.items = {Fact("{A}", 9.9, true, 9.9, 10.0), Fact("{B}", 13.9, true, 13.9, 14.0)};
        const FollowPlan p = PlanItemFollow(in);
        CHECK(Count(p, FollowKind::Remove) == 2 && p.record.events.empty());
        CheckIdempotent(in, p);
    }
    // Suppressed / deleted: populated Remove; attached Detach + report.
    {
        FollowInput in = OneSound(Ownership::Attached);
        in.items = {Fact("{A}", 9.9, false, 0, 0)};
        const FollowPlan p = PlanItemFollow(in);
        CHECK(Count(p, FollowKind::Detach) == 1 && Count(p, FollowKind::Remove) == 0);
        CHECK(p.report.size() == 1 && p.report[0].kind == ReportKind::Detached);
        CHECK(p.record.events.empty());
        CheckIdempotent(in, p);
    }
    // Trimmed out / back.
    {
        FollowInput in = OneSound(Ownership::Populated);
        in.events = {Ev("r1", 1.0, 0.0, false, false)};
        in.items = {Fact("{A}", 9.9, true, 9.9, 10.0)};
        const FollowPlan p = PlanItemFollow(in);
        CHECK(Count(p, FollowKind::Mute) == 1 && Count(p, FollowKind::Move) == 0);
        const FollowAction* s = Find(p, FollowKind::Store);
        CHECK(s && s->store.mute && Near(s->store.pt, 10.0));
        CheckIdempotent(in, p);
        // Back in.
        FollowInput back = in;
        back.events = {Ev("r1", 1.0, 10.0)};
        back.items[0].muted = true;
        back.items[0].store.mute = true;
        const FollowPlan q = PlanItemFollow(back);
        CHECK(Count(q, FollowKind::Unmute) == 1);
        const FollowAction* s2 = Find(q, FollowKind::Store);
        CHECK(s2 && !s2->store.mute);
        CheckIdempotent(back, q);
        // A user mute (no flag) outside: untouched.
        FollowInput um = in;
        um.items[0].muted = true;
        const FollowPlan u = PlanItemFollow(um);
        CHECK(u.actions.empty());
        // ... and back in: still untouched.
        um.events = {Ev("r1", 1.0, 10.0)};
        CHECK(PlanItemFollow(um).actions.empty());
        // Attached outside: left alone.
        FollowInput at = OneSound(Ownership::Attached);
        at.events = {Ev("r1", 1.0, 0.0, false, false)};
        at.items = {Fact("{A}", 9.9, true, 9.9, 10.0)};
        CHECK(PlanItemFollow(at).actions.empty());
    }
    // Hand delete: populated -> Deleted, no action; attached -> dropped.
    {
        FollowInput in = OneSound(Ownership::Populated);
        in.events = {Ev("r1", 1.0, 10.0)};
        const FollowPlan p = PlanItemFollow(in);
        CHECK(p.actions.empty() && p.record_changed);
        CHECK(p.record.events[0].placed.size() == 1 && p.record.events[0].placed[0].own == Ownership::Deleted);
        CheckIdempotent(in, p);
        FollowInput at = OneSound(Ownership::Attached);
        at.events = {Ev("r1", 1.0, 10.0)};
        const FollowPlan q = PlanItemFollow(at);
        CHECK(q.actions.empty() && q.record.events[0].placed.empty());
        CheckIdempotent(at, q);
    }
    // Track move: track_guid updated, nothing else.
    {
        FollowInput in = OneSound(Ownership::Populated);
        in.events = {Ev("r1", 1.0, 10.0)};
        in.items = {Fact("{A}", 9.9, true, 9.9, 10.0)};
        in.items[0].track_guid = "{T2}";
        const FollowPlan p = PlanItemFollow(in);
        CHECK(p.actions.empty() && p.record_changed && p.record.events[0].placed[0].track_guid == "{T2}");
        CheckIdempotent(in, p);
    }
    // Record and facts with GUIDs of different case: the same item.
    {
        FollowInput in = OneSound(Ownership::Populated);
        in.rec.events[0].placed[0].item_guid = "{aa}";
        in.events = {Ev("r1", 1.0, 11.0)};
        in.items = {Fact("{AA}", 9.9, true, 9.9, 10.0)};
        in.items[0].tag_owner = "{owner}";
        const FollowPlan p = PlanItemFollow(in);
        const FollowAction* m = Find(p, FollowKind::Move);
        CHECK(m && Near(m->project_t, 10.9) && !p.record_changed);
        CheckIdempotent(in, p);
    }
    // A placement tagged for another owner on an item seen before (a copy's record restored by
    // undo): dropped, never moved, stored nor removed.
    {
        FollowInput in = OneSound(Ownership::Populated);
        in.owner_guid = "{COPY}";
        in.events = {Ev("r1", 1.0, 11.0)};
        in.items = {Fact("{A}", 9.9, true, 9.9, 10.0)};
        const FollowPlan p = PlanItemFollow(in);
        CHECK(p.actions.empty() && p.record_changed && !p.populate_copy);
        CHECK(p.record.events.size() == 1 && p.record.events[0].placed.empty());
        CheckIdempotent(in, p);
        // Its event gone: still nothing done to the original's item.
        in.events.clear();
        CHECK(PlanItemFollow(in).actions.empty());
    }
    // First sight while the event is outside the item: nothing, until it is back in.
    {
        FollowInput in = OneSound(Ownership::Populated);
        in.events = {Ev("r1", 1.0, 0.0, false, false)};
        in.items = {Fact("{A}", 9.9, false, 0, 0)};
        const FollowPlan p = PlanItemFollow(in);
        CHECK(p.actions.empty() && !p.record_changed);
        in.events = {Ev("r1", 1.0, 11.0)};
        const FollowPlan q = PlanItemFollow(in);
        CHECK(Count(q, FollowKind::Move) == 0 && q.actions.size() == 1);
        CHECK(q.actions.size() == 1 && Near(q.actions[0].store.pos, 9.9) && Near(q.actions[0].store.pt, 11.0));
    }
    // Retag on an attached sound: its tag follows the event, it is never moved nor stored.
    {
        FollowInput in = OneSound(Ownership::Attached);
        in.events = {Ev("r1", 1.02, 11.0)};
        in.items = {Fact("{A}", 9.9, true, 9.9, 10.0)};
        const FollowPlan p = PlanItemFollow(in);
        CHECK(p.actions.size() == 1 && p.actions[0].kind == FollowKind::Retag && Near(p.actions[0].tag_t, 1.02));
        CheckIdempotent(in, p);
    }
    // Orphan rule with an attached sound: one Detach, two report lines.
    {
        FollowInput in = OneSound(Ownership::Attached);
        in.rule_rids.clear();
        in.events = {Ev("r1", 1.0, 10.0)};
        in.items = {Fact("{A}", 9.9, true, 9.9, 10.0)};
        const FollowPlan p = PlanItemFollow(in);
        CHECK(Count(p, FollowKind::Detach) == 1 && p.actions.size() == 1 && p.report.size() == 2);
        CHECK(p.record.rules.empty() && p.record.events.empty());
        CheckIdempotent(in, p);
    }
    // Orphan rid: rule removed, events gone, report.
    {
        FollowInput in = OneSound(Ownership::Populated);
        in.rule_rids.clear();
        in.events = {Ev("r1", 1.0, 10.0)};
        in.items = {Fact("{A}", 9.9, true, 9.9, 10.0)};
        const FollowPlan p = PlanItemFollow(in);
        CHECK(p.record.rules.empty() && p.record.events.empty());
        CHECK(Count(p, FollowKind::Remove) == 1);
        CHECK(p.report.size() == 1
              && p.report[0].text == "Assignment removed: rule no longer exists (Footsteps)");
        CheckIdempotent(in, p);
    }
    // Copy: new item, all placements foreign -> dropped, reshuffle unless keep, populate flag.
    {
        FollowInput in = OneSound(Ownership::Populated);
        in.owner_guid = "{COPY}";
        in.appeared = true;
        in.copy_seed = FollowGuidHash("{COPY}");
        in.events = {Ev("r1", 1.0, 10.0)};
        in.items = {Fact("{A}", 9.9, true, 9.9, 10.0)};
        FollowPlan p = PlanItemFollow(in);
        CHECK(p.populate_copy && p.record_changed && p.actions.empty());
        CHECK(p.record.events[0].placed.empty());
        CHECK(p.record.seed == in.copy_seed);
        in.keep_draw = true;
        p = PlanItemFollow(in);
        CHECK(p.populate_copy && p.record.events[0].draws.size() == 1 && p.record.events[0].draws[0].pick == 2);
        // Not appeared: never a copy.
        in.appeared = false;
        CHECK(!PlanItemFollow(in).populate_copy);
        // Appeared but owns its placements: not a copy.
        FollowInput own = OneSound(Ownership::Populated);
        own.appeared = true;
        own.events = {Ev("r1", 1.0, 10.0)};
        own.items = {Fact("{A}", 9.9, true, 9.9, 10.0)};
        CHECK(!PlanItemFollow(own).populate_copy);
        CHECK(FollowGuidHash("{ab}") == FollowGuidHash("{AB}"));
    }
    // First sight: store only.
    {
        FollowInput in = OneSound(Ownership::Populated);
        in.events = {Ev("r1", 1.0, 11.0)};
        in.items = {Fact("{A}", 9.9, false, 0, 0)};
        const FollowPlan p = PlanItemFollow(in);
        CHECK(p.actions.size() == 1 && p.actions[0].kind == FollowKind::Store);
        CHECK(Near(p.actions[0].store.pos, 9.9) && Near(p.actions[0].store.pt, 11.0));
        CheckIdempotent(in, p);
    }
    // Owner gone: populated removed, attached detached + report, a foreign tag left alone.
    {
        std::vector<SoundPlacement> placed(3);
        placed[0].item_guid = "{A}";
        placed[1].item_guid = "{B}";
        placed[1].own = Ownership::Attached;
        placed[2].item_guid = "{C}";
        std::vector<FollowItemFacts> items = {Fact("{A}", 1, true, 1, 1), Fact("{B}", 1, true, 1, 1),
                                              Fact("{C}", 1, true, 1, 1)};
        items[2].tag_owner = "{OTHER}";
        const FollowPlan p = PlanOwnerGone(kOwner, placed, items);
        CHECK(Count(p, FollowKind::Remove) == 1 && Count(p, FollowKind::Detach) == 1 && p.report.size() == 1);
        // A Deleted placement is skipped even when its GUID is present.
        placed[0].own = Ownership::Deleted;
        const FollowPlan q = PlanOwnerGone(kOwner, placed, items);
        CHECK(Count(q, FollowKind::Remove) == 0 && Count(q, FollowKind::Detach) == 1);
    }
    // CommittedFollowEvents.
    {
        ItemRules r;
        r.blocks.resize(4);
        r.blocks[3].rid = "d";
        r.blocks[0].rid = "a";
        r.blocks[1].rid = "b";
        r.blocks[1].enabled = false;
        r.blocks[2].rid = "";
        auto add = [&](double t, EventKind k, int b) {
            EventEntry e;
            e.t = t;
            e.kind = k;
            e.block = b;
            r.events.push_back(e);
        };
        add(1.0, EventKind::Detected, 0);
        add(1.02, EventKind::Suppress, 0);  // masks 1.0
        add(2.0, EventKind::Detected, 0);
        add(2.5, EventKind::User, 0);
        add(3.0, EventKind::Detected, 1);  // disabled rule
        add(3.5, EventKind::Detected, 2);  // no rid
        add(4.0, EventKind::Detected, 7);  // bad index
        add(9.0, EventKind::Detected, 0);  // outside the item
        add(9.01, EventKind::Suppress, 3);  // another rule's suppression: masks nothing of rule 0
        ItemClipMap m;
        m.item_pos = 10.0;
        m.item_len = 5.0;
        m.clip_len = 20.0;
        const std::vector<FollowEvent> ev = CommittedFollowEvents(r, m);
        CHECK(ev.size() == 3);
        if (ev.size() == 3) {
            CHECK(Near(ev[0].t, 2.0) && ev[0].in_item && Near(ev[0].project_t, 12.0) && !ev[0].user);
            CHECK(Near(ev[1].t, 2.5) && ev[1].user && ev[1].rid == "a");
            CHECK(Near(ev[2].t, 9.0) && !ev[2].in_item && ev[2].rid == "a");
        }
    }
    // Fix 2026-10-12: a freshly populated record plans no Remove on the next tick. Populate (13-8)
    // places on the live event list (detections not committed yet + User entries), one sound per
    // loop pass; the follow reads the same list (SoundEventsOf), all passes.
    {
        ItemRules r;
        Block b;
        b.rid = "a";
        b.enabled = true;
        r.blocks.push_back(b);
        EventEntry u;  // a User event; no Detected entry: no Commit since detection changed
        u.t = 0.8;
        u.kind = EventKind::User;
        u.block = 0;
        r.events.push_back(u);
        std::vector<Event> dets(1);
        dets[0].time_s = 0.25;
        dets[0].block = 0;
        ItemClipMap m;  // loops a 1 s clip three times from 10 s
        m.item_pos = 10.0;
        m.item_len = 3.0;
        m.clip_len = 1.0;
        m.loop = true;
        const std::vector<FollowEvent> ev = SoundEventsOf(BuildEventList(dets, r.events, r.blocks.size()), r.blocks, m);
        CHECK(ev.size() == 2);
        for (const FollowEvent& e : ev) {
            const std::vector<double> pt = ClipToProjectTimes(m, e.t);  // what Populate placed on
            CHECK(e.passes.size() == 3 && pt.size() == 3);
            for (size_t k = 0; k < e.passes.size() && k < pt.size(); ++k)
                CHECK(e.passes[k].id == static_cast<long long>(k) && Near(e.passes[k].project_t, pt[k]));
        }
        CHECK(ev.size() == 2 && ev[0].rid == "a" && !ev[0].user && ev[1].user);

        // The record as Populate wrote it: a node with a 30 ms Blend offset, placed at pass + 30 ms.
        FollowInput in;
        in.owner_guid = kOwner;
        RuleSounds rs;
        rs.rid = "a";
        rs.library = "L1";
        in.rec.rules.push_back(rs);
        in.rule_rids = {"a"};
        in.blend_ms = {{"n1", 30.0}};
        int g = 0;
        for (const FollowEvent& e : ev) {
            EventSounds es;
            es.rid = "a";
            es.t = e.t;
            for (const FollowPass& fp : e.passes) {
                char guid[16];
                std::snprintf(guid, sizeof(guid), "{P%d}", g++);
                SoundPlacement p;
                p.item_guid = guid;
                p.node = "n1";
                p.own = Ownership::Populated;
                p.track_guid = "{T1}";
                es.placed.push_back(p);
                in.items.push_back(Fact(guid, fp.project_t + 0.03, false, 0.0, 0.0, e.t));
            }
            in.rec.events.push_back(es);
        }
        in.events = ev;
        const FollowPlan p = PlanItemFollow(in);
        CHECK(Count(p, FollowKind::Remove) == 0 && Count(p, FollowKind::Detach) == 0);
        CHECK(Count(p, FollowKind::Move) == 0 && Count(p, FollowKind::Mute) == 0);
        CHECK(Count(p, FollowKind::Store) == 6 && !p.record_changed);
        for (const FollowAction& a : p.actions) {  // each sound keeps its own pass
            if (a.kind != FollowKind::Store) continue;
            const int k = std::atoi(a.item_guid.c_str() + 2) % 3;
            CHECK(a.store.has_pass && a.store.pass == k);
            CHECK(Near(a.store.pt, 10.0 + k + (std::atoi(a.item_guid.c_str() + 2) < 3 ? 0.25 : 0.8)));
        }
        CheckIdempotent(in, p);

        // The old source (the record's last Commit) had no event for the detection: it removed.
        FollowInput old_in = in;
        old_in.events = CommittedFollowEvents(r, m);
        CHECK(Count(PlanItemFollow(old_in), FollowKind::Remove) == 3);

        // The stores written, then the item changes.
        FollowInput next = in;
        for (const FollowAction& a : p.actions)
            for (FollowItemFacts& f : next.items)
                if (a.kind == FollowKind::Store && SameFollowGuid(f.item_guid, a.item_guid)) {
                    f.has_store = true;
                    f.store = a.store;
                }
        auto replan = [&](const ItemClipMap& mm) {
            FollowInput x = next;
            x.events = SoundEventsOf(BuildEventList(dets, r.events, r.blocks.size()), r.blocks, mm);
            return PlanItemFollow(x);
        };
        {  // moved 2 s later: every sound moves 2 s, each on its pass
            ItemClipMap mm = m;
            mm.item_pos = 12.0;
            const FollowPlan q = replan(mm);
            CHECK(Count(q, FollowKind::Move) == 6 && Count(q, FollowKind::Remove) == 0);
            for (const FollowAction& a : q.actions)
                if (a.kind == FollowKind::Move)
                    for (const FollowItemFacts& f : next.items)
                        if (f.item_guid == a.item_guid) CHECK(Near(a.project_t, f.pos + 2.0));
        }
        {  // played twice as fast: pass k of 0.25 lands at 10 + (k + 0.25) / 2
            ItemClipMap mm = m;
            mm.rate = 2.0;
            const FollowPlan q = replan(mm);
            CHECK(Count(q, FollowKind::Remove) == 0 && Count(q, FollowKind::Mute) == 0);
            const FollowAction* a1 = nullptr;
            for (const FollowAction& a : q.actions)
                if (a.kind == FollowKind::Move && a.item_guid == "{P1}") a1 = &a;
            CHECK(a1 && Near(a1->project_t, 10.0 + 1.25 / 2.0 + 0.03));
        }
        {  // trimmed at its start by one loop: the first pass's sounds are muted, the others stay
            ItemClipMap mm = m;
            mm.item_pos = 11.0;
            mm.item_len = 2.0;
            mm.start_offs = 1.0;
            const FollowPlan q = replan(mm);
            CHECK(Count(q, FollowKind::Mute) == 2 && Count(q, FollowKind::Move) == 0);
            CHECK(Count(q, FollowKind::Remove) == 0);
        }
        // An older store line (no pass) is read, and the pass round-trips.
        FollowStore st;
        CHECK(ParseFollowStore("RAVSNDFOLLOW 1 pos=1 pt=2 pass=-3", &st) && st.has_pass && st.pass == -3);
        CHECK(ParseFollowStore(FormatFollowStore(st), &st) && st.pass == -3);
        CHECK(ParseFollowStore("RAVSNDFOLLOW 1 pos=1 pt=2", &st) && !st.has_pass);
    }

    // PlanFollow (13-2 wrapper) still links and plans.
    {
        FollowInput in = OneSound(Ownership::Populated);
        const FollowPlan p = PlanFollow(in.rec, {Ev("r1", 1.0, 10.0)}, {"{A}"});
        CHECK(Count(p, FollowKind::Store) == 1);
    }

    if (g_fail) {
        std::printf("sound_follow_plan_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("sound_follow_plan_test: all passed\n");
    return 0;
}
