// SPDX-License-Identifier: MIT
//
// Host test of the item's event list (src/event_list.h, story 10-4): merge and +-30 ms
// masking, orphans, rule-index remap, the first pass only, Apply's plan and signature, the
// foreign-marker twin check, a preset load clearing the corrections. No REAPER, no Windows: any C++17 compiler.

#include "event_list.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace rav;

namespace {

int g_fails = 0;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);        \
            ++g_fails;                                              \
        }                                                           \
    } while (0)

bool Near(double a, double b, double tol = 1e-9)
{
    return std::fabs(a - b) < tol;
}

Event Det(int block, double t, double strength = 1.0)
{
    Event e;
    e.block = block;
    e.time_s = t;
    e.strength = strength;
    e.speed = 2.0;
    return e;
}

Block Rule(const char* name, uint32_t color = 0x1000000u | 0x5F9EDDu)
{
    Block b;
    b.marker = name;
    b.color = color;
    return b;
}

int Count(const std::vector<ShownEvent>& l, ShownKind k)
{
    int n = 0;
    for (const ShownEvent& e : l)
        if (e.kind == k) ++n;
    return n;
}

}  // namespace

int main()
{
    // ---- Merge: detections, suppressions (+-30 ms, inclusive), orphans, user events --------
    {
        const std::vector<Event> det = {Det(0, 1.0), Det(0, 2.0), Det(1, 1.0), Det(0, 3.0)};
        std::vector<EventEntry> entries = {
            MakeSuppression(0, 1.030),          // masks rule 0's 1.0 (edge: exactly 30 ms)
            MakeSuppression(0, 2.0301),         // just past the window: nothing masked -> orphan
            MakeSuppression(1, 3.0),            // rule 1 has nothing at 3.0 -> orphan (rule 0's 3.0 is not its)
            MakeUserEvent(0, 1.5, 0.7, 1.2),    // the user's own
        };
        EventEntry snap;  // Apply's snapshot: never shown
        snap.kind = EventKind::Detected;
        snap.t = 4.0;
        entries.push_back(snap);
        EventEntry future;  // a kind this version does not know: ignored
        future.kind = EventKind::Other;
        future.t = 4.5;
        entries.push_back(future);
        entries.push_back(MakeUserEvent(5, 1.0, 0, 0));  // a rule that does not exist: not shown

        const std::vector<ShownEvent> l = BuildEventList(det, entries, 2);
        CHECK(l.size() == 7);
        CHECK(Count(l, ShownKind::Detected) == 3);
        CHECK(Count(l, ShownKind::Suppressed) == 1);
        CHECK(Count(l, ShownKind::Orphan) == 2);
        CHECK(Count(l, ShownKind::User) == 1);
        for (size_t i = 1; i < l.size(); ++i) CHECK(l[i - 1].t <= l[i].t);
        for (const ShownEvent& e : l) {
            if (e.kind == ShownKind::Suppressed) CHECK(e.block == 0 && Near(e.t, 1.0) && e.entry == 0 && e.detection == 0);
            if (e.kind == ShownKind::User) CHECK(Near(e.t, 1.5) && e.entry == 3 && Near(e.strength, 0.7) && Near(e.speed, 1.2));
            if (e.kind == ShownKind::Orphan) CHECK(e.entry == 1 || e.entry == 2);
            if (e.kind == ShownKind::Detected && e.block == 1) CHECK(Near(e.t, 1.0) && e.detection == 2);
        }
        // Edge, the other side: -30 ms masks, -30.1 ms does not.
        CHECK(Count(BuildEventList({Det(0, 1.0)}, {MakeSuppression(0, 0.970)}, 1), ShownKind::Suppressed) == 1);
        const std::vector<ShownEvent> o = BuildEventList({Det(0, 1.0)}, {MakeSuppression(0, 0.9699)}, 1);
        CHECK(Count(o, ShownKind::Detected) == 1 && Count(o, ShownKind::Orphan) == 1);
        // One suppression masks every detection of its rule in the window.
        CHECK(Count(BuildEventList({Det(0, 1.0), Det(0, 1.02)}, {MakeSuppression(0, 1.01)}, 1), ShownKind::Suppressed) == 2);
        // Detection never moves a user event: the user event is shown at its own time whatever is detected.
        const std::vector<ShownEvent> u = BuildEventList({Det(0, 1.49)}, {MakeUserEvent(0, 1.5, 0, 0)}, 1);
        CHECK(u.size() == 2 && Count(u, ShownKind::User) == 1 && Count(u, ShownKind::Detected) == 1);
    }

    // ---- Rule delete / duplicate: remap ------------------------------------------------------
    {
        std::vector<EventEntry> e = {MakeUserEvent(0, 1, 0, 0), MakeSuppression(0, 2), MakeUserEvent(1, 3, 0, 0),
                                     MakeSuppression(2, 4), MakeUserEvent(2, 5, 0, 0)};
        EventEntry other;
        other.kind = EventKind::Other;
        other.block = 0;
        e.push_back(other);
        // Rule 0 of 3 deleted: its user events and suppressions go; rule 1's now point at 0, rule 2's at 1.
        std::vector<EventEntry> d = e;
        RemapEventBlocks(d, BlockMapForDelete(3, 0));
        CHECK(d.size() == 4);
        CHECK(d[0].block == 0 && Near(d[0].t, 3));
        CHECK(d[1].block == 1 && d[1].kind == EventKind::Suppress && d[2].block == 1);
        CHECK(d[3].kind == EventKind::Other);  // unknown kind: left as it is
        // Rule 1 of 3 deleted (the spec's "rule 1 of 3"): rule 2's events now point at index 1.
        d = e;
        RemapEventBlocks(d, BlockMapForDelete(3, 1));
        CHECK(d.size() == 5 && d[0].block == 0 && d[1].block == 0 && d[2].block == 1 && d[3].block == 1);
        // Rule 0 duplicated (the copy inserted at 1): rules 1 and 2 move down.
        d = e;
        RemapEventBlocks(d, BlockMapForInsert(3, 1));
        CHECK(d.size() == 6 && d[0].block == 0 && d[1].block == 0 && d[2].block == 2 && d[3].block == 3 && d[4].block == 3);
        // A record entry read without `block` (block 0) gets one when it moves.
        EventEntry nb;
        nb.kind = EventKind::User;
        nb.has_block = false;
        nb.block = 0;
        std::vector<EventEntry> one = {nb};
        RemapEventBlocks(one, BlockMapForInsert(1, 0));
        CHECK(one.size() == 1 && one[0].block == 1 && one[0].has_block);
    }

    // ---- First pass only, inside the item's bounds ------------------------------------------
    {
        ItemClipMap m;
        m.item_pos = 10.0;
        m.item_len = 5.0;  // longer than the clip, looped: RAV shows one pass then holds
        m.clip_len = 2.0;
        m.loop = true;
        double p = 0.0, c = 0.0;
        CHECK(FirstPassProjectTime(m, 0.5, &p) && Near(p, 10.5));
        CHECK(!FirstPassProjectTime(m, 2.5, &p));  // beyond the clip
        CHECK(FirstPassClipTime(m, 11.0, &c) && Near(c, 1.0));
        CHECK(!FirstPassClipTime(m, 12.5, &c));  // the second loop pass: no clip time
        ClipPass fp;
        CHECK(FirstClipPass(m, &fp) && Near(fp.p0, 10.0) && Near(fp.p1, 12.0));
        // Start offset and play rate; the item trimmed at its start.
        m.start_offs = 0.5;
        m.rate = 2.0;
        m.loop = false;
        CHECK(FirstPassProjectTime(m, 1.5, &p) && Near(p, 10.5));
        CHECK(!FirstPassProjectTime(m, 0.25, &p));  // before the item's start (trimmed away)
        // An item shorter than the clip: what lies after its end is not written.
        m = ItemClipMap{};
        m.item_pos = 0.0;
        m.item_len = 1.0;
        m.clip_len = 2.0;
        CHECK(FirstPassProjectTime(m, 1.0, &p) && Near(p, 1.0));
        CHECK(!FirstPassProjectTime(m, 1.2, &p));
        CHECK(!FirstClipPass(ItemClipMap{}, &fp));
    }

    // ---- Apply's plan, signature and record -------------------------------------------------
    {
        std::vector<Block> blocks = {Rule("Footstep L"), Rule(" Footstep R \n", 0)};
        ItemClipMap m;
        m.item_pos = 10.0;
        m.item_len = 3.0;
        m.clip_len = 2.0;
        m.loop = true;
        std::vector<EventEntry> entries = {MakeSuppression(0, 0.5), MakeUserEvent(1, 0.8, 0.3, 0.4),
                                           MakeUserEvent(0, 1.9, 0, 0)};
        const std::vector<Event> det = {Det(0, 0.5), Det(0, 1.0), Det(1, 1.2), Det(0, 2.5)};
        const std::vector<ShownEvent> l = BuildEventList(det, entries, blocks.size());
        std::vector<PlannedMarker> plan = PlanMarkers(l, blocks, m);
        // 0.5 suppressed; 2.5 is beyond the first pass; 3 detections/user events remain + 1.9 user.
        CHECK(plan.size() == 4);
        if (plan.size() == 4) {
            CHECK(Near(plan[0].clip_t, 0.8) && plan[0].user && plan[0].name == "Footstep R" && plan[0].color == 0);
            CHECK(Near(plan[1].clip_t, 1.0) && !plan[1].user && Near(plan[1].project_t, 11.0) && plan[1].name == "Footstep L");
            CHECK(plan[1].color == (0x1000000u | 0x5F9EDDu));
        }
        // A rule switched off writes nothing.
        blocks[1].enabled = false;
        CHECK(PlanMarkers(l, blocks, m).size() == 2);
        blocks[1].enabled = true;

        // Signature: stable, changes with the option, the events, the names; project times count
        // only when project markers are written.
        const std::string sb = MarkerSignature(plan, MarkerMode::Both);
        CHECK(sb.size() == 16 && sb == MarkerSignature(plan, MarkerMode::Both));
        CHECK(sb != MarkerSignature(plan, MarkerMode::Take));
        std::vector<PlannedMarker> moved = plan;
        for (PlannedMarker& p : moved) p.project_t += 1.0;  // the item moved
        CHECK(MarkerSignature(moved, MarkerMode::Take) == MarkerSignature(plan, MarkerMode::Take));
        CHECK(MarkerSignature(moved, MarkerMode::Both) != sb);
        std::vector<PlannedMarker> renamed = plan;
        renamed[0].name = "X";
        CHECK(MarkerSignature(renamed, MarkerMode::Both) != sb);

        ItemRules r;
        r.events = entries;
        CHECK(!MarkersUpToDate(r, plan, MarkerMode::Both));
        RecordApplied(r, plan, MarkerMode::Both);
        CHECK(MarkersUpToDate(r, plan, MarkerMode::Both));
        CHECK(!MarkersUpToDate(r, plan, MarkerMode::Take));  // the option changed
        CHECK(!MarkersUpToDate(r, renamed, MarkerMode::Both));
        // The snapshot: the detections written, first; the user events and suppressions stay.
        int snap = 0;
        for (const EventEntry& e : r.events)
            if (e.kind == EventKind::Detected) ++snap;
        CHECK(snap == 2 && r.events.size() == 5 && r.events[0].kind == EventKind::Detected);
        // A second Apply replaces the snapshot.
        RecordApplied(r, {plan[1]}, MarkerMode::Take);
        snap = 0;
        for (const EventEntry& e : r.events)
            if (e.kind == EventKind::Detected) ++snap;
        CHECK(snap == 1 && r.applied.mode == MarkerMode::Take);
        // The snapshot is never shown again as events (the live detections are).
        CHECK(BuildEventList({}, r.events, 2).size() == 3);
        // It round-trips through the record.
        ItemRules back;
        CHECK(ParseItemRules(SerializeItemRules(r), &back) && MarkersUpToDate(back, {plan[1]}, MarkerMode::Take));
    }

    // ---- Foreign marker twin (same name, same time +-1 ms) -----------------------------------
    {
        const std::vector<ExistingMarker> ex = {{1.0, "1"}, {2.0, "Footstep L"}, {3.0, "Other"}};
        CHECK(FindTwinMarker(ex, 1.0, "1") == 0);
        CHECK(FindTwinMarker(ex, 1.0009, "1") == 0);
        CHECK(FindTwinMarker(ex, 1.0011, "1") == -1);
        CHECK(FindTwinMarker(ex, 2.0, "Footstep R") == -1);  // another name
        CHECK(FindTwinMarker(ex, 1.9995, "Footstep L") == 1);
        CHECK(FindTwinMarker({}, 1.0, "1") == -1);
    }

    // ---- Preset loaded: the corrections go, the owned markers and unknown entries stay -------
    {
        ItemRules r;
        r.blocks = {Rule("A")};
        EventEntry snap;
        snap.kind = EventKind::Detected;
        EventEntry other;
        other.kind = EventKind::Other;
        r.events = {snap, MakeUserEvent(0, 1, 0, 0), other, MakeSuppression(0, 2)};
        r.has_applied = true;
        r.applied.sig = "X";
        TakeMarkerRef tm;
        tm.name = "A";
        r.tmarkers = {tm};
        ProjectMarkerRef pm;
        pm.guid = "{G}";
        r.pmarkers = {pm};
        ClearCorrections(r);
        CHECK(r.events.size() == 1 && r.events[0].kind == EventKind::Other);
        CHECK(!r.has_applied && r.applied.sig.empty());
        CHECK(r.tmarkers.size() == 1 && r.pmarkers.size() == 1 && r.blocks.size() == 1);
    }

    if (g_fails) {
        std::printf("event_list_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("event_list_test: all passed\n");
    return 0;
}
