// SPDX-License-Identifier: MIT
//
// Host test of src/sound_follow_plan.h (story 13-9): every row of the spec's I/O matrix, the
// follow store's line, CommittedFollowEvents.

#include "sound_follow_plan.h"

#include <cmath>
#include <cstdio>
#include <set>
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
            if (a.kind == FollowKind::Retag) {
                f.tag_t = a.tag_t;
                if (!a.tag_owner.empty()) f.tag_owner = a.tag_owner;
            }
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
    // 13-20 / 13-23 (D7): a part cut by an excluded zone is placed as two items of one placement, tagged
    // ao / sy from the anchor piece: an event move moves both by the same amount, the gap kept, both on
    // the same pass at first sight.
    {
        FollowInput in = OneSound(Ownership::Populated);
        SoundPlacement second = in.rec.events[0].placed[0];
        second.item_guid = "{B}";
        in.rec.events[0].placed.push_back(second);
        in.events = {Ev("r1", 1.0, 11.0)};
        in.items = {Fact("{A}", 9.982, true, 9.982, 10.0), Fact("{B}", 10.102, true, 10.102, 10.0)};
        in.items[0].tag_has_anchor = in.items[1].tag_has_anchor = true;
        in.items[0].tag_sy = in.items[1].tag_sy = 0.018;
        in.items[1].tag_ao = 0.12;
        const FollowPlan p = PlanItemFollow(in);
        CHECK(Count(p, FollowKind::Move) == 2);
        for (const FollowAction& a : p.actions)
            if (a.kind == FollowKind::Move)
                CHECK(Near(a.project_t, SameFollowGuid(a.item_guid, "{A}") ? 10.982 : 11.102, 1e-9));
        CheckIdempotent(in, p);
        // First sight (no store), two passes 0.2 s apart: both pieces find the pass of their anchor.
        FollowInput first = in;
        FollowEvent e = Ev("r1", 1.0, 10.0);
        e.passes = {FollowPass{0, 10.0}, FollowPass{1, 10.2}};
        first.events = {e};
        first.items[0] = Fact("{A}", 9.982, false, 0, 0);
        first.items[1] = Fact("{B}", 10.102, false, 0, 0);
        first.items[0].tag_has_anchor = first.items[1].tag_has_anchor = true;
        first.items[0].tag_sy = first.items[1].tag_sy = 0.018;
        first.items[1].tag_ao = 0.12;  // by its own position + sync (10.12) it would be nearer pass 1
        const FollowPlan q = PlanItemFollow(first);
        int on0 = 0;
        for (const FollowAction& a : q.actions)
            if (a.kind == FollowKind::Store && a.store.has_pass && a.store.pass == 0) ++on0;
        CHECK(on0 == 2 && Count(q, FollowKind::Move) == 0);
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
    // 13-68: an attached sound follows its event (retagged and moved), like a placed one.
    {
        FollowInput in = OneSound(Ownership::Attached);
        in.events = {Ev("r1", 1.02, 11.0)};
        in.items = {Fact("{A}", 9.9, true, 9.9, 10.0)};
        const FollowPlan p = PlanItemFollow(in);
        CHECK(Count(p, FollowKind::Retag) == 1 && Near(Find(p, FollowKind::Retag)->tag_t, 1.02));
        CHECK(Count(p, FollowKind::Move) == 1 && Near(Find(p, FollowKind::Move)->project_t, 10.9));
        CHECK(Count(p, FollowKind::Mute) == 0);
        // First sight: stored, nothing moved.
        FollowInput fs = OneSound(Ownership::Attached);
        fs.events = {Ev("r1", 1.0, 10.0)};
        fs.items = {Fact("{A}", 9.9, false, 0, 0)};
        const FollowPlan q = PlanItemFollow(fs);
        CHECK(Count(q, FollowKind::Move) == 0 && Count(q, FollowKind::Store) == 1);
        // Moved by hand: the nudge is kept in the record, nothing moved.
        FollowInput hm = OneSound(Ownership::Attached);
        hm.events = {Ev("r1", 1.0, 10.0)};
        hm.items = {Fact("{A}", 10.2, true, 9.9, 10.0)};
        const FollowPlan r = PlanItemFollow(hm);
        CHECK(Count(r, FollowKind::Move) == 0 && Near(r.record.events[0].placed[0].offset_s, 0.3));
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

        // 13-31: the rule's own entry wins over the node's: on a role event the layer's offset was dropped
        // (placed at pass + 0) although the node's combined offset is 600 ms; each sound keeps its pass.
        {
            FollowInput r0 = in;
            r0.blend_ms = {{"n1", 600.0}, {std::string("a") + '\x1f' + "n1", 0.0}};
            int i = 0;
            for (const FollowEvent& e : ev)
                for (const FollowPass& fp : e.passes) r0.items[static_cast<size_t>(i++)].pos = fp.project_t;
            const FollowPlan q = PlanItemFollow(r0);
            CHECK(Count(q, FollowKind::Move) == 0 && Count(q, FollowKind::Mute) == 0 && Count(q, FollowKind::Remove) == 0);
            CHECK(Count(q, FollowKind::Store) == 6);
            for (const FollowAction& a : q.actions) {
                if (a.kind != FollowKind::Store) continue;
                CHECK(a.store.has_pass && a.store.pass == std::atoi(a.item_guid.c_str() + 2) % 3);
            }
        }

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
        // 13-56: a looped item stretched or shortened in REAPER (Source loop mode).
        auto replan_loop = [&](const ItemClipMap& mm, FollowInput x) {
            x.owner_loops = mm.loop;
            x.events = SoundEventsOf(BuildEventList(dets, r.events, r.blocks.size()), r.blocks, mm);
            return PlanItemFollow(x);
        };
        {  // shortened to two loops: the third pass's sounds removed and dropped, the others kept
            ItemClipMap mm = m;
            mm.item_len = 2.0;
            const FollowPlan q = replan_loop(mm, next);
            CHECK(Count(q, FollowKind::Remove) == 2 && Count(q, FollowKind::Mute) == 0 && Count(q, FollowKind::Move) == 0);
            for (const FollowAction& a : q.actions)
                if (a.kind == FollowKind::Remove) CHECK(a.item_guid == "{P2}" || a.item_guid == "{P5}");
            CHECK(q.record_changed && !q.populate_passes);
            CHECK(q.record.events.size() == 2);
            for (const EventSounds& es : q.record.events) {
                CHECK(es.placed.size() == 2);
                for (const SoundPlacement& sp : es.placed) CHECK(sp.item_guid != "{P2}" && sp.item_guid != "{P5}");
            }
            FollowInput x = next;
            x.owner_loops = true;
            x.events = SoundEventsOf(BuildEventList(dets, r.events, r.blocks.size()), r.blocks, mm);
            CheckIdempotent(x, q);
            // Cut in the middle of the third loop: the event at 0.25 still shows there, 0.8 does not.
            mm.item_len = 2.5;
            const FollowPlan h = replan_loop(mm, next);
            CHECK(Count(h, FollowKind::Remove) == 1 && Find(h, FollowKind::Remove)->item_guid == "{P5}");
            // The same on an item that does not loop (its first pass only): muted as before, nothing removed.
            FollowInput y = next;
            y.events = SoundEventsOf(BuildEventList(dets, r.events, r.blocks.size()), r.blocks, mm);
            CHECK(!y.owner_loops && Count(PlanItemFollow(y), FollowKind::Remove) == 0);
        }
        {  // trimmed at its start by one loop: the first pass's sounds are muted (as before), not removed
            ItemClipMap mm = m;
            mm.item_pos = 11.0;
            mm.item_len = 2.0;
            mm.start_offs = 1.0;
            const FollowPlan q = replan_loop(mm, next);
            CHECK(Count(q, FollowKind::Mute) == 2 && Count(q, FollowKind::Remove) == 0 && !q.populate_passes);
        }
        {  // stretched to five loops: nothing moves, the new passes are to be populated
            ItemClipMap mm = m;
            mm.item_len = 5.0;
            const FollowPlan q = replan_loop(mm, next);
            CHECK(q.actions.empty() && !q.record_changed && q.populate_passes);
            // Not looped: never.
            FollowInput y = next;
            y.events = SoundEventsOf(BuildEventList(dets, r.events, r.blocks.size()), r.blocks, mm);
            CHECK(!PlanItemFollow(y).populate_passes);
            // Once filled (a sound on every pass of each event): nothing more to populate.
            FollowInput full = next;
            full.owner_loops = true;
            full.events = SoundEventsOf(BuildEventList(dets, r.events, r.blocks.size()), r.blocks, mm);
            int g = 100;
            for (size_t k = 0; k < full.events.size(); ++k)
                for (const FollowPass& fp : full.events[k].passes) {
                    if (fp.id < 3) continue;
                    const std::string guid = "{P" + std::to_string(g++) + "}";
                    SoundPlacement sp;
                    sp.item_guid = guid;
                    sp.node = "n1";
                    sp.own = Ownership::Populated;
                    sp.track_guid = "{T1}";
                    full.rec.events[k].placed.push_back(sp);
                    full.items.push_back(Fact(guid.c_str(), fp.project_t + 0.03, false, 0.0, 0.0, full.events[k].t));
                }
            const FollowPlan f = PlanItemFollow(full);
            CHECK(!f.populate_passes && Count(f, FollowKind::Remove) == 0 && Count(f, FollowKind::Move) == 0);
            // 13-59: New variation per pass: a pass that drew another node (a Random container) is
            // filled too; with Same, that node's missing passes still ask for a fill.
            {
                FollowInput other = full;
                for (EventSounds& es : other.rec.events)
                    for (SoundPlacement& sp : es.placed)
                        if (sp.item_guid.compare(0, 3, "{P1") == 0 && sp.item_guid.size() > 4) sp.node = "n2";
                other.pass_draws = true;
                CHECK(!PlanItemFollow(other).populate_passes);
                other.pass_draws = false;
                CHECK(PlanItemFollow(other).populate_passes);
            }
            {  // New variation per pass, a pass with no sound at all: filled.
                FollowInput x = next;
                x.pass_draws = true;
                CHECK(replan_loop(mm, x).populate_passes);
            }
            // A sound deleted by hand on that event: its node is never placed again, nothing to fill.
            FollowInput del = next;
            for (EventSounds& es : del.rec.events) {
                SoundPlacement gone;
                gone.item_guid = "{GONE}";
                gone.node = "n1";
                gone.own = Ownership::Deleted;
                es.placed.push_back(gone);
            }
            CHECK(!replan_loop(mm, del).populate_passes);
            // Only one of the two events has one: the other still fills its passes.
            del = next;
            SoundPlacement gone;
            gone.item_guid = "{GONE}";
            gone.node = "n1";
            gone.own = Ownership::Deleted;
            del.rec.events[0].placed.push_back(gone);
            CHECK(replan_loop(mm, del).populate_passes);
            // An event with an attached or spread sound is left to the user (Populate skips it).
            FollowInput att = next;
            for (EventSounds& es : att.rec.events) {
                SoundPlacement a;
                a.item_guid = "{ATT}";
                a.node = "n1";
                a.own = Ownership::Spread;
                es.placed.push_back(a);
            }
            CHECK(!replan_loop(mm, att).populate_passes);
            // A disabled rule's events are paused: nothing to fill.
            FollowInput dis = next;
            dis.disabled_rids = {"a"};
            CHECK(!replan_loop(mm, dis).populate_passes);
        }
        // The plan applied as the REAPER side would (CheckIdempotent's rules), the record taken.
        auto applied = [](FollowInput x, const FollowPlan& q) {
            x.rec = q.record;
            for (const FollowAction& a : q.actions)
                for (FollowItemFacts& f : x.items) {
                    if (!SameFollowGuid(f.item_guid, a.item_guid)) continue;
                    if (a.kind == FollowKind::Move) f.pos = a.project_t;
                    if (a.kind == FollowKind::Store) { f.has_store = true; f.store = a.store; }
                    if (a.kind == FollowKind::Mute) f.muted = true;
                    if (a.kind == FollowKind::Unmute) f.muted = false;
                    if (a.kind == FollowKind::Remove) f.present = false;
                }
            x.appeared = false;
            return x;
        };
        {  // review: a spread sound on a pass cut off is muted (Populate never places it again), then
           // unmuted when the item is stretched again; an attached one is never touched
            ItemClipMap mm = m;
            mm.item_len = 2.0;
            FollowInput x = next;
            x.rec.events[0].placed[2].own = Ownership::Spread;     // {P2}, pass 2
            x.rec.events[1].placed[2].own = Ownership::Attached;   // {P5}, pass 2
            const FollowPlan q = replan_loop(mm, x);
            CHECK(Count(q, FollowKind::Remove) == 0 && Count(q, FollowKind::Detach) == 0);
            CHECK(Count(q, FollowKind::Mute) == 1 && Find(q, FollowKind::Mute)->item_guid == "{P2}");
            FollowInput after = applied(x, q);
            after.events = SoundEventsOf(BuildEventList(dets, r.events, r.blocks.size()), r.blocks, mm);
            after.owner_loops = true;
            CheckIdempotent(after, replan_loop(mm, after));
            // Stretched back to three loops: unmuted, in place.
            const FollowPlan b = replan_loop(m, after);
            CHECK(Count(b, FollowKind::Unmute) == 1 && Find(b, FollowKind::Unmute)->item_guid == "{P2}");
            CHECK(Count(b, FollowKind::Remove) == 0);
        }
        {  // review: the last live sound of a node in an event, on a cut pass, is muted (not removed),
           // so stretching again fills the event: event 0's n1 lives on pass 2 only (passes 0 and 1
           // play n2).
            ItemClipMap mm = m;
            mm.item_len = 2.0;
            FollowInput x = next;
            x.rec.events[0].placed[0].node = "n2";  // {P0}, pass 0
            x.rec.events[0].placed[1].node = "n2";  // {P1}, pass 1
            const std::set<std::string> last =
                LastOfNodeKept(x.rec.events[0], SoundEventsOf(BuildEventList(dets, r.events, r.blocks.size()), r.blocks, mm)[0],
                               [&] {
                                   FollowInput y = x;
                                   y.owner_loops = true;
                                   return y;
                               }());
            CHECK(last.size() == 1 && last.count("{P2}"));
            const FollowPlan q = replan_loop(mm, x);
            CHECK(Count(q, FollowKind::Remove) == 1 && Find(q, FollowKind::Remove)->item_guid == "{P5}");
            CHECK(Count(q, FollowKind::Mute) == 1 && Find(q, FollowKind::Mute)->item_guid == "{P2}");
            // Stretched again to five loops: {P2} back in its pass (unmuted), and the passes to fill.
            FollowInput after = applied(x, q);
            ItemClipMap five = m;
            five.item_len = 5.0;
            const FollowPlan b = replan_loop(five, after);
            CHECK(Count(b, FollowKind::Unmute) == 1 && Find(b, FollowKind::Unmute)->item_guid == "{P2}");
            CHECK(b.populate_passes);
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

    // Fix C 2026-10-12 (1): a user unmute of a sound the follow muted is respected until the
    // event is back inside; the next exit mutes it again.
    {
        FollowInput in = OneSound(Ownership::Populated);
        in.events = {Ev("r1", 1.0, 0.0, false, false)};
        in.items = {Fact("{A}", 9.9, true, 9.9, 10.0)};
        in.items[0].store.mute = true;  // the follow muted it, the user unmuted it
        const FollowPlan p = PlanItemFollow(in);
        CHECK(Count(p, FollowKind::Mute) == 0);
        const FollowAction* s = Find(p, FollowKind::Store);
        CHECK(s && s->store.hand && !s->store.mute);
        CheckIdempotent(in, p);
        FollowInput h = in;
        h.items[0].store = s->store;
        CHECK(PlanItemFollow(h).actions.empty());
        // Back inside: no Unmute (not muted by the follow), hand cleared.
        h.events = {Ev("r1", 1.0, 10.0)};
        const FollowPlan q = PlanItemFollow(h);
        CHECK(Count(q, FollowKind::Unmute) == 0 && Count(q, FollowKind::Mute) == 0);
        const FollowAction* s2 = Find(q, FollowKind::Store);
        CHECK(s2 && !s2->store.hand && !s2->store.mute);
        // Out again: muted again.
        h.items[0].store = s2->store;
        h.events = {Ev("r1", 1.0, 0.0, false, false)};
        const FollowPlan r = PlanItemFollow(h);
        CHECK(Count(r, FollowKind::Mute) == 1);
        // Store line round trip with hand.
        FollowStore st, rd;
        st.hand = true;
        CHECK(ParseFollowStore(FormatFollowStore(st), &rd) && rd.hand && !rd.mute);
        CHECK(ParseFollowStore("RAVSNDFOLLOW 1 pos=1 pt=2 mute=1", &rd) && !rd.hand && rd.mute);
    }
    // Fix C (2): a disabled rule keeps its sounds, muted; enabled again, unmuted.
    {
        // Its Detected event is not detected while disabled: held, muted, nothing removed.
        FollowInput in = OneSound(Ownership::Populated);
        in.disabled_rids = {"r1"};
        in.rec.events[0].placed[0].offset_s = 0.05;
        in.items = {Fact("{A}", 9.95, true, 9.95, 10.0)};
        const FollowPlan p = PlanItemFollow(in);
        CHECK(Count(p, FollowKind::Remove) == 0 && Count(p, FollowKind::Detach) == 0 && Count(p, FollowKind::Mute) == 1);
        CHECK(!p.record_changed && p.record.events.size() == 1 && Near(p.record.events[0].placed[0].offset_s, 0.05));
        const FollowAction* s = Find(p, FollowKind::Store);
        CHECK(s && s->store.mute && Near(s->store.pos, 9.95) && Near(s->store.pt, 10.0));
        CheckIdempotent(in, p);
        // Enabled again, its event back: unmuted, not moved, nudge kept.
        FollowInput on = in;
        on.disabled_rids.clear();
        on.events = {Ev("r1", 1.0, 10.0)};
        on.items[0].muted = true;
        on.items[0].store = s->store;
        const FollowPlan q = PlanItemFollow(on);
        CHECK(Count(q, FollowKind::Unmute) == 1 && Count(q, FollowKind::Move) == 0 && !q.record_changed);
        CheckIdempotent(on, q);
        // Unmuted by the user while disabled: left alone; enabled: not muted, not unmuted.
        FollowInput um = in;
        um.items[0].store.mute = true;
        const FollowPlan u = PlanItemFollow(um);
        CHECK(Count(u, FollowKind::Mute) == 0 && Find(u, FollowKind::Store) && Find(u, FollowKind::Store)->store.hand);
        // A User event of the disabled rule (still listed): followed, muted.
        FollowInput ue = in;
        ue.events = {Ev("r1", 1.0, 11.0, true)};
        ue.events[0].enabled = false;
        const FollowPlan v = PlanItemFollow(ue);
        CHECK(Count(v, FollowKind::Move) == 1 && Count(v, FollowKind::Mute) == 1);
        CheckIdempotent(ue, v);
        // An attached sound: muted while disabled (kept attached), unmuted when enabled.
        FollowInput at = OneSound(Ownership::Attached);
        at.disabled_rids = {"r1"};
        at.items = {Fact("{A}", 9.9, false, 0, 0)};
        const FollowPlan w = PlanItemFollow(at);
        CHECK(Count(w, FollowKind::Mute) == 1 && Count(w, FollowKind::Detach) == 0 && w.record.events.size() == 1);
        const FollowAction* ws = Find(w, FollowKind::Store);
        CHECK(ws && ws->store.mute);
        CheckIdempotent(at, w);
        FollowInput at2 = at;
        at2.disabled_rids.clear();
        at2.events = {Ev("r1", 1.0, 10.0)};
        at2.items[0].muted = true;
        at2.items[0].has_store = true;
        at2.items[0].store = ws->store;
        const FollowPlan x = PlanItemFollow(at2);
        CHECK(Count(x, FollowKind::Unmute) == 1 && Count(x, FollowKind::Move) == 0);
        CheckIdempotent(at2, x);
        // SoundEventsOf marks a disabled rule's events (Populate skips them).
        ItemRules r;
        r.blocks.resize(1);
        r.blocks[0].rid = "a";
        r.blocks[0].enabled = false;
        EventEntry e;
        e.t = 1.0;
        e.kind = EventKind::User;
        e.block = 0;
        r.events.push_back(e);
        ItemClipMap m;
        m.item_pos = 10.0;
        m.item_len = 5.0;
        m.clip_len = 5.0;
        const std::vector<FollowEvent> ev = SoundEventsOf(BuildEventList({}, r.events, 1), r.blocks, m);
        CHECK(ev.size() == 1 && !ev[0].enabled);
    }
    // Fix C (3): a copy made together with its sound items.
    {
        auto copy_in = [](bool keep) {
            FollowInput in = OneSound(Ownership::Populated);
            in.owner_guid = "{COPY}";
            in.appeared = true;
            in.keep_draw = keep;
            in.copy_seed = FollowGuidHash("{COPY}");
            in.owner_pos = 20.0;
            in.owner_positions = {{kOwner, 9.0}, {"{COPY}", 20.0}};
            in.events = {Ev("r1", 1.0, 21.0)};
            in.items = {Fact("{A}", 9.9, true, 9.9, 10.0)};
            in.items[0].tag_rid = "r1";
            in.items[0].tag_node = "n1";
            FollowItemFacts c = Fact("{A2}", 20.9, true, 9.9, 10.0);  // copied with its tag and store
            c.tag_rid = "r1";
            c.tag_node = "n1";
            FollowItemFacts far = Fact("{X}", 30.0, true, 30.0, 30.0);  // tagged, not explained by the copy
            in.strays = {far, c};
            return in;
        };
        // Keep the draw: adopted, retagged for the copy, nothing new placed.
        FollowInput in = copy_in(true);
        const FollowPlan p = PlanItemFollow(in);
        CHECK(!p.populate_copy && p.record_changed);
        CHECK(p.record.events[0].placed.size() == 1 && p.record.events[0].placed[0].item_guid == "{A2}");
        CHECK(p.record.events[0].draws.size() == 1 && p.record.events[0].draws[0].pick == 2);
        const FollowAction* rt = Find(p, FollowKind::Retag);
        CHECK(rt && rt->item_guid == "{A2}" && rt->tag_owner == "{COPY}" && p.actions.size() == 1);
        // Next tick: followed as its own (copied store, both moved: no nudge, no move).
        FollowInput nx = in;
        nx.appeared = false;
        nx.rec = p.record;
        nx.items = {in.strays[1]};
        nx.items[0].tag_owner = "{COPY}";
        const FollowPlan q = PlanItemFollow(nx);
        CHECK(Count(q, FollowKind::Move) == 0 && Count(q, FollowKind::Remove) == 0 && !q.record_changed);
        CHECK(Near(q.record.events[0].placed[0].offset_s, 0.0));
        CheckIdempotent(nx, q);
        // Keep the draw off: the copied populated sound removed, a new draw placed.
        const FollowPlan r = PlanItemFollow(copy_in(false));
        CHECK(r.populate_copy && Count(r, FollowKind::Remove) == 1 && Find(r, FollowKind::Remove)->item_guid == "{A2}");
        CHECK(r.record.events[0].placed.empty() && r.record.events[0].draws.empty());
        // ... an attached one is adopted even then.
        FollowInput at = copy_in(false);
        at.rec.events[0].placed[0].own = Ownership::Attached;
        const FollowPlan s = PlanItemFollow(at);
        CHECK(Count(s, FollowKind::Remove) == 0 && s.record.events[0].placed.size() == 1 &&
              s.record.events[0].placed[0].item_guid == "{A2}");
        // A stray at another place (not the copy's): never touched.
        FollowInput nf = copy_in(true);
        nf.strays.pop_back();
        const FollowPlan t = PlanItemFollow(nf);
        CHECK(t.actions.empty() && t.populate_copy && t.record.events[0].placed.empty());
    }
    // Fix C (4): cut and paste = a move: record kept, items retagged / adopted.
    {
        // Only the animation item cut: its sound items stay, tagged for the GUID it had.
        FollowInput in = OneSound(Ownership::Populated);
        in.owner_guid = "{NEW}";
        in.appeared = true;
        in.moved_from = kOwner;
        in.moved_delta = 5.0;
        in.rec.events[0].placed[0].offset_s = 0.1;
        in.events = {Ev("r1", 1.0, 15.0)};
        in.items = {Fact("{A}", 10.0, true, 10.0, 10.0)};
        in.moved_items = in.items;
        const FollowPlan p = PlanItemFollow(in);
        CHECK(!p.populate_copy && Count(p, FollowKind::Remove) == 0 && Count(p, FollowKind::Detach) == 0);
        const FollowAction* rt = Find(p, FollowKind::Retag);
        CHECK(rt && rt->tag_owner == "{NEW}");
        const FollowAction* mv = Find(p, FollowKind::Move);
        CHECK(mv && Near(mv->project_t, 15.0));
        CHECK(p.record.events[0].draws.size() == 1 && Near(p.record.events[0].placed[0].offset_s, 0.1));
        CheckIdempotent(in, p);
        // Cut together with its sound item (pasted with a new GUID): adopted, nudge kept, no move.
        FollowInput tg = in;
        tg.items.clear();
        FollowItemFacts c = Fact("{A9}", 15.0, true, 10.0, 10.0);
        tg.strays = {c};
        const FollowPlan q = PlanItemFollow(tg);
        CHECK(!q.populate_copy && q.record_changed && Count(q, FollowKind::Move) == 0);
        CHECK(q.record.events[0].placed.size() == 1 && q.record.events[0].placed[0].item_guid == "{A9}" &&
              q.record.events[0].placed[0].own == Ownership::Populated && Near(q.record.events[0].placed[0].offset_s, 0.1));
        CHECK(Find(q, FollowKind::Retag) && Find(q, FollowKind::Retag)->tag_owner == "{NEW}");
        // MatchMovedOwners: same rules record and file, one on each side.
        std::vector<MoveCandidate> gone = {{"{G1}", "rules A", "a.fbx"}, {"{G2}", "rules B", "b.fbx"}};
        std::vector<MoveCandidate> came = {{"{N1}", "rules B", "b.fbx"}, {"{N2}", "rules A", "c.fbx"}};
        auto pr = MatchMovedOwners(gone, came);
        CHECK(pr.size() == 1 && pr[0].first == 1 && pr[0].second == 0);
        came.push_back({"{N3}", "rules B", "b.fbx"});  // two candidates: not a move
        CHECK(MatchMovedOwners(gone, came).empty());
        CHECK(MatchMovedOwners({{"{G}", "", ""}}, {{"{N}", "", ""}}).empty());
    }

    // Fix C, review (9): an unmute by hand given while the event was outside does not hold
    // through a later rule disable (the reason is kept: hand=1 outside, hand=2 disabled).
    {
        FollowInput in = OneSound(Ownership::Populated);
        in.events = {Ev("r1", 1.0, 0.0, false, false)};
        in.items = {Fact("{A}", 9.9, true, 9.9, 10.0)};
        in.items[0].store.hand = kMuteOutside;  // unmuted by hand while outside
        CHECK(PlanItemFollow(in).actions.empty());
        FollowInput d = in;
        d.events[0].enabled = false;
        d.disabled_rids = {"r1"};
        const FollowPlan p = PlanItemFollow(d);
        CHECK(Count(p, FollowKind::Mute) == 1);
        const FollowAction* s = Find(p, FollowKind::Store);
        CHECK(s && s->store.mute && s->store.hand == 0);
        CheckIdempotent(d, p);
        // Unmuted by hand while disabled: hand=2, kept while disabled.
        FollowInput u = d;
        u.items[0].store = s->store;  // muted by the follow, the user unmuted it (muted false)
        const FollowPlan q = PlanItemFollow(u);
        CHECK(Count(q, FollowKind::Mute) == 0 && Find(q, FollowKind::Store) &&
              Find(q, FollowKind::Store)->store.hand == kMuteDisabled);
        FollowStore rd;
        CHECK(ParseFollowStore(FormatFollowStore(Find(q, FollowKind::Store)->store), &rd) && rd.hand == kMuteDisabled);
        CHECK(ParseFollowStore("RAVSNDFOLLOW 1 pos=1 pt=2 hand=1", &rd) && rd.hand == kMuteOutside);
    }
    // Fix C, review (10): at first sight a folder pile member finds its pass by the pile's anchor
    // (tag ao / sy), as Populate does: one pile never splits across close passes.
    {
        FollowInput in = OneSound(Ownership::Populated);
        FollowEvent e = Ev("r1", 1.0, 10.0);
        e.passes = {FollowPass{0, 10.0}, FollowPass{1, 12.0}};
        in.events = {e};
        in.items = {Fact("{A}", 11.2, false, 0, 0)};  // by its own position: nearer pass 1
        FollowInput own = in;
        const FollowAction* s0 = Find(PlanItemFollow(own), FollowKind::Store);
        CHECK(s0 && s0->store.has_pass && s0->store.pass == 1);
        in.items[0].tag_has_anchor = true;
        in.items[0].tag_ao = 1.5;  // anchored at 9.7: pass 0
        in.items[0].tag_sy = 0.0;
        const FollowPlan p = PlanItemFollow(in);
        const FollowAction* s = Find(p, FollowKind::Store);
        CHECK(s && s->store.has_pass && s->store.pass == 0 && Near(s->store.pt, 10.0));
        CHECK(Count(p, FollowKind::Move) == 0);
    }
    // Fix C, review (1): a duplicate of another identical instance is never a move.
    {
        const MoveCandidate g{"{G}", "rules A", "a.fbx", {"{S1}"}, {"{G}"}};
        CHECK(MatchMovedOwners({g}, {{"{N}", "rules A", "a.fbx", {"{S1}"}, {"{G}"}}}).size() == 1);   // its paste
        CHECK(MatchMovedOwners({g}, {{"{N}", "rules A", "a.fbx", {"{s1}"}, {}}}).size() == 1);        // cut with its sounds
        CHECK(MatchMovedOwners({g}, {{"{N}", "rules A", "a.fbx", {"{S2}"}, {"{B}"}}}).empty());       // Ctrl+D of B
        CHECK(MatchMovedOwners({g}, {{"{N}", "rules A", "a.fbx", {"{S1}"}, {"{B}"}}}).empty());       // B's items
        CHECK(MatchMovedOwners({g}, {{"{N}", "rules A", "a.fbx", {"{S1}", "{S2}"}, {"{G}"}}}).empty());
        // Items tagged for a GUID it had before (a paste undone): still its own.
        const MoveCandidate g2{"{G2}", "rules A", "a.fbx", {"{S1}"}, {"{G2}", "{G}"}};
        CHECK(MatchMovedOwners({g2}, {{"{N}", "rules A", "a.fbx", {"{S1}"}, {"{G}"}}}).size() == 1);
    }
    // Fix C, review (2+3): only a cut waits for its paste.
    {
        CHECK(IsCutUndoName("Cut items"));
        CHECK(IsCutUndoName("cut selected area of items"));
        CHECK(IsCutUndoName("Ripple CUT"));
        CHECK(IsCutUndoName("Cut items/tracks/envelope points"));
        CHECK(!IsCutUndoName("Execute script"));
        CHECK(!IsCutUndoName("Remove items"));
        CHECK(!IsCutUndoName("Paste items/tracks"));
        CHECK(!IsCutUndoName(""));
        CHECK(!IsCutUndoName(nullptr));
    }
    // Fix C, review (4): the facts once a plan is applied (an owner's snapshot after its tick).
    {
        std::vector<FollowItemFacts> f = {Fact("{A}", 9.9, true, 9.9, 10.0), Fact("{B}", 5.0, true, 5.0, 5.0)};
        FollowPlan p;
        FollowAction mv;
        mv.kind = FollowKind::Move;
        mv.item_guid = "{a}";
        mv.project_t = 11.9;
        p.actions.push_back(mv);
        FollowAction rt;
        rt.kind = FollowKind::Retag;
        rt.item_guid = "{B}";
        rt.tag_t = 2.0;
        rt.tag_owner = "{NEW}";
        p.actions.push_back(rt);
        FollowAction mu;
        mu.kind = FollowKind::Mute;
        mu.item_guid = "{B}";
        p.actions.push_back(mu);
        FollowAction rm;
        rm.kind = FollowKind::Remove;
        rm.item_guid = "{X}";
        p.actions.push_back(rm);
        FactsAfterPlan(p, &f);
        CHECK(Near(f[0].pos, 11.9) && !f[0].muted && f[0].present);
        CHECK(Near(f[1].pos, 5.0) && f[1].muted && f[1].tag_owner == "{NEW}" && Near(f[1].tag_t, 2.0));
        // The owner's follow moved its sound, then it was cut and pasted 5 s later: the pasted
        // copy of the sound is found at the moved position + 5.
        FollowInput in = OneSound(Ownership::Populated);
        in.owner_guid = "{NEW}";
        in.appeared = true;
        in.moved_from = kOwner;
        in.moved_delta = 5.0;
        in.events = {Ev("r1", 1.0, 17.0)};
        in.moved_items = {f[0]};
        FollowItemFacts c = Fact("{A9}", 16.9, true, 11.9, 12.0);
        in.strays = {c};
        const FollowPlan q = PlanItemFollow(in);
        CHECK(q.record.events[0].placed.size() == 1 && q.record.events[0].placed[0].item_guid == "{A9}");
    }
    // Fix C, review (5): after a paste undone, the items tagged for the GUID the owner had stay its
    // own: a move retags them, and when it is gone they are handled.
    {
        FollowInput in = OneSound(Ownership::Populated);
        in.owner_guid = "{N2}";
        in.appeared = true;
        in.moved_from = "{N1}";
        in.moved_also = {kOwner};
        in.events = {Ev("r1", 1.0, 10.0)};
        in.items = {Fact("{A}", 9.9, true, 9.9, 10.0)};  // tagged for kOwner (the paste's retag undone)
        in.moved_items = in.items;
        const FollowPlan p = PlanItemFollow(in);
        const FollowAction* rt = Find(p, FollowKind::Retag);
        CHECK(rt && rt->item_guid == "{A}" && rt->tag_owner == "{N2}");
        std::vector<SoundPlacement> placed = in.rec.events[0].placed;
        CHECK(PlanOwnerGone("{N1}", placed, in.items).actions.empty());
        const FollowPlan g = PlanOwnerGone("{N1}", placed, in.items, {kOwner});
        CHECK(Count(g, FollowKind::Remove) == 1);
    }

    // Fix C, review 2 (1): a copy whose source is gone (pasted a second time after a cut, or
    // copied, its source deleted, then pasted): the sound items pasted with it, tagged for the gone
    // source, are adopted (Keep the draw on) or removed (off): never two sets.
    {
        auto gone_src = [](bool keep) {
            FollowInput in = OneSound(Ownership::Populated);
            in.owner_guid = "{X2}";
            in.appeared = true;
            in.keep_draw = keep;
            in.copy_seed = FollowGuidHash("{X2}");
            in.owner_pos = 20.0;
            in.owner_len = 4.0;
            in.owner_positions = {{"{X1}", 9.0}, {"{X2}", 20.0}, {"{B}", 40.0}};  // kOwner gone
            in.events = {Ev("r1", 1.0, 21.0)};
            in.items = {};  // {A}, its record's item, is gone with its owner
            FollowItemFacts c = Fact("{A3}", 20.9, true, 9.9, 10.0);  // pasted with it, tagged for kOwner
            c.tag_rid = "r1";
            c.tag_node = "n1";
            FollowItemFacts b = c;  // the same place, tagged for an owner in the project: never touched
            b.item_guid = "{B3}";
            b.tag_owner = "{B}";
            in.strays = {b, c};
            return in;
        };
        // Remembered: the gone owner as last seen (at 9.0, its sound at 9.9).
        FollowInput in = gone_src(true);
        in.remembered = {FollowOwnerMemory{kOwner, 9.0, {Fact("{A}", 9.9, true, 9.9, 10.0)}}};
        const FollowPlan p = PlanItemFollow(in);
        CHECK(!p.populate_copy && p.record.events[0].placed.size() == 1 && p.record.events[0].placed[0].item_guid == "{A3}");
        CHECK(Find(p, FollowKind::Retag) && Find(p, FollowKind::Retag)->item_guid == "{A3}" &&
              Find(p, FollowKind::Retag)->tag_owner == "{X2}" && p.actions.size() == 1);
        FollowInput off = gone_src(false);
        off.remembered = in.remembered;
        const FollowPlan q = PlanItemFollow(off);
        CHECK(q.populate_copy && Count(q, FollowKind::Remove) == 1 && Find(q, FollowKind::Remove)->item_guid == "{A3}");
        // Nothing remembered: where Populate places it on the copy (the event at 21.0, its nudge).
        FollowInput g = gone_src(true);
        g.rec.events[0].placed[0].offset_s = -0.1;
        const FollowPlan r = PlanItemFollow(g);
        CHECK(!r.populate_copy && r.record.events[0].placed.size() == 1 && r.record.events[0].placed[0].item_guid == "{A3}" &&
              Near(r.record.events[0].placed[0].offset_s, -0.1));
        FollowInput goff = gone_src(false);
        goff.rec.events[0].placed[0].offset_s = -0.1;
        const FollowPlan r2 = PlanItemFollow(goff);
        CHECK(r2.populate_copy && Count(r2, FollowKind::Remove) == 1 && Find(r2, FollowKind::Remove)->item_guid == "{A3}");
        // ... a pile member by its anchor (tag ao / sy).
        FollowInput pile = gone_src(true);
        pile.strays[1].pos = 21.5;
        pile.strays[1].tag_has_anchor = true;
        pile.strays[1].tag_ao = 0.6;
        pile.strays[1].tag_sy = 0.1;  // anchor 20.9 + sync 0.1 = 21.0
        CHECK(PlanItemFollow(pile).record.events[0].placed.size() == 1);
        // Not at its place (no nudge recorded): not the copy's, nothing touched.
        const FollowPlan t = PlanItemFollow(gone_src(true));
        CHECK(t.actions.empty() && t.populate_copy && t.record.events[0].placed.empty());
        // Another rule, or tagged for an owner in the project: never.
        FollowInput wr = gone_src(true);
        wr.rec.events[0].placed[0].offset_s = -0.1;
        wr.strays[1].tag_rid = "r2";
        CHECK(PlanItemFollow(wr).actions.empty());
        FollowInput pr = gone_src(true);
        pr.rec.events[0].placed[0].offset_s = -0.1;
        pr.owner_positions.push_back({kOwner, 9.0});
        CHECK(PlanItemFollow(pr).actions.empty());
        // An attached sound: inside the copy's span, and only when alone.
        FollowInput at = gone_src(false);
        at.rec.events[0].placed[0].own = Ownership::Attached;
        at.strays[1].pos = 22.5;
        const FollowPlan u = PlanItemFollow(at);
        CHECK(Count(u, FollowKind::Remove) == 0 && u.record.events[0].placed.size() == 1 &&
              u.record.events[0].placed[0].item_guid == "{A3}");
        FollowInput at2 = at;
        FollowItemFacts c2 = at.strays[1];
        c2.item_guid = "{A4}";
        c2.pos = 23.0;
        at2.strays.push_back(c2);
        CHECK(PlanItemFollow(at2).record.events[0].placed.empty());
        at.strays[1].pos = 30.0;  // outside the copy
        CHECK(PlanItemFollow(at).record.events[0].placed.empty());
    }
    // Fix C, review 2 (2): an animation item gone and back (undo, redo) appears again.
    {
        std::set<std::string> seen;
        std::vector<bool>     a = UpdateSeenOwners(&seen, {"{A}"}, false);
        CHECK(a.size() == 1 && !a[0]);
        a = UpdateSeenOwners(&seen, {"{A}", "{X}"}, true);
        CHECK(!a[0] && a[1]);
        a = UpdateSeenOwners(&seen, {"{A}"}, true);  // the paste undone
        CHECK(!a[0] && seen.size() == 1);
        a = UpdateSeenOwners(&seen, {"{A}", "{X}"}, true);  // redone: appears again
        CHECK(!a[0] && a[1]);

        // Cut and paste, the paste undone (X1 waits for a paste, as the cut is the last undo point
        // again), then redone: X1 comes back as REAPER's redo has it (its record lists A's sound
        // items, its pasted sound items tagged for A). It matches the GUID it had (A), not itself.
        const MoveCandidate x1{"{X1}", "rules A", "a.fbx", {"{S1P}"}, {"{X1}", kOwner}};
        const MoveCandidate a1{kOwner, "rules A", "a.fbx", {"{S1}"}, {"{X1}", kOwner}};
        const MoveCandidate redo{"{X1}", "rules A", "a.fbx", {"{S1}"}, {}};
        const auto pr = MatchMovedOwners({x1, a1}, {redo});
        CHECK(pr.size() == 1 && pr[0].first == 1 && pr[0].second == 0);
        FollowInput in = OneSound(Ownership::Populated);
        in.owner_guid = "{X1}";
        in.appeared = true;
        in.moved_from = kOwner;
        in.moved_delta = 5.0;
        in.events = {Ev("r1", 1.0, 15.0)};
        in.moved_items = {Fact("{A}", 9.9, true, 9.9, 10.0)};
        in.strays = {Fact("{S1P}", 14.9, true, 9.9, 10.0)};  // tagged for kOwner
        const FollowPlan p = PlanItemFollow(in);
        CHECK(!p.populate_copy && p.record.events[0].placed.size() == 1 && p.record.events[0].placed[0].item_guid == "{S1P}");
        CHECK(Find(p, FollowKind::Retag) && Find(p, FollowKind::Retag)->tag_owner == "{X1}");
        // The cut undone: A back, matched with itself (nothing moved).
        const MoveCandidate back{kOwner, "rules A", "a.fbx", {"{S1}"}, {kOwner}};
        CHECK(MatchMovedOwners({a1}, {back}).size() == 1);

        // Copy and paste (Keep the draw on), undone, redone: the redo appears again and adopts
        // the pasted sound items again, as the first paste did.
        std::set<std::string> seen2 = {kOwner};
        CHECK(UpdateSeenOwners(&seen2, {kOwner, "{COPY}"}, true)[1]);
        CHECK(!UpdateSeenOwners(&seen2, {kOwner}, true)[0]);
        const std::vector<bool> again = UpdateSeenOwners(&seen2, {kOwner, "{COPY}"}, true);
        FollowInput cp = OneSound(Ownership::Populated);
        cp.owner_guid = "{COPY}";
        cp.appeared = again[1];
        cp.keep_draw = true;
        cp.owner_pos = 20.0;
        cp.owner_positions = {{kOwner, 9.0}, {"{COPY}", 20.0}};
        cp.events = {Ev("r1", 1.0, 21.0)};
        cp.items = {Fact("{A}", 9.9, true, 9.9, 10.0)};
        FollowItemFacts c = Fact("{A2}", 20.9, true, 9.9, 10.0);
        cp.strays = {c};
        const FollowPlan cq = PlanItemFollow(cp);
        CHECK(!cq.populate_copy && cq.record.events[0].placed.size() == 1 && cq.record.events[0].placed[0].item_guid == "{A2}");
    }
    // Fix C, review 2 (3+4): a selection undo point keeps a cut waiting; localized names.
    {
        CHECK(IsSelectionUndoName("Change media item selection"));
        CHECK(IsSelectionUndoName("Unselect all items"));
        CHECK(IsSelectionUndoName("SELECT ALL ITEMS"));
        CHECK(IsSelectionUndoName("Modifier la s\xC3\xA9lection des \xC3\xA9l\xC3\xA9ments"));
        CHECK(IsSelectionUndoName("S\xC3\x89LECTION"));
        CHECK(!IsSelectionUndoName("Move items"));
        CHECK(!IsSelectionUndoName(nullptr));
        CHECK(IsSelectionUndoName("Auswahl ge\xC3\xA4ndert", {"Auswahl"}));
        CHECK(!IsSelectionUndoName("Auswahl ge\xC3\xA4ndert"));
        CHECK(IsCutUndoName("Couper les \xC3\xA9l\xC3\xA9ments"));
        CHECK(!IsCutUndoName("D\xC3\xA9" "couper"));
        CHECK(!IsCutUndoName("Elemente ausschneiden"));
        CHECK(IsCutUndoName("Elemente ausschneiden", {"Ausschneiden"}));
        CHECK(!IsCutUndoName("Elemente einf\xC3\xBCgen", {"Ausschneiden", ""}));
    }

    if (g_fail) {
        std::printf("sound_follow_plan_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("sound_follow_plan_test: all passed\n");
    return 0;
}
