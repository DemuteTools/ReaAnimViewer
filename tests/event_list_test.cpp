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
        // 0.5 suppressed; 3 detections/user events remain + 1.9 user; 2.5 is beyond the first
        // pass: kept, outside the item (10-4b).
        CHECK(plan.size() == 5);
        if (plan.size() == 5) {
            CHECK(Near(plan[0].clip_t, 0.8) && plan[0].user && plan[0].name == "Footstep R" && plan[0].color == 0);
            CHECK(Near(plan[1].clip_t, 1.0) && !plan[1].user && Near(plan[1].project_t, 11.0) && plan[1].name == "Footstep L");
            CHECK(plan[1].color == (0x1000000u | 0x5F9EDDu));
            CHECK(plan[0].in_item && plan[1].in_item && plan[3].in_item);
            CHECK(Near(plan[4].clip_t, 2.5) && !plan[4].in_item);
        }
        // A rule switched off writes nothing.
        blocks[1].enabled = false;
        CHECK(PlanMarkers(l, blocks, m).size() == 3);
        blocks[1].enabled = true;

        // Signature: stable, changes with the option, the events, the names; 10-4b: never with
        // where the item sits (a move or a trim keeps "markers up to date").
        const std::string sb = MarkerSignature(plan, MarkerMode::Both);
        CHECK(sb.size() == 16 && sb == MarkerSignature(plan, MarkerMode::Both));
        CHECK(sb != MarkerSignature(plan, MarkerMode::Take));
        std::vector<PlannedMarker> moved = plan;
        for (PlannedMarker& p : moved) p.project_t += 1.0;  // the item moved
        CHECK(MarkerSignature(moved, MarkerMode::Take) == MarkerSignature(plan, MarkerMode::Take));
        CHECK(MarkerSignature(moved, MarkerMode::Both) == sb);
        {
            // Planned again after a real move, and after a trim that leaves an event outside.
            ItemClipMap mv = m;
            mv.item_pos = 12.0;
            CHECK(MarkerSignature(PlanMarkers(l, blocks, mv), MarkerMode::Both) == sb);
            ItemClipMap tr = m;
            tr.item_len = 0.9;
            const std::vector<PlannedMarker> trimmed = PlanMarkers(l, blocks, tr);
            CHECK(trimmed.size() == plan.size());
            int inside = 0;
            for (const PlannedMarker& p : trimmed) inside += p.in_item ? 1 : 0;
            CHECK(inside == 1);  // only 0.8 is before 10.9
            // Placement-free in every mode: a trim / extension keeps "up to date" (the mirror
            // hides and shows the project markers; take markers, written for every event, show
            // again by themselves).
            for (MarkerMode md : {MarkerMode::Take, MarkerMode::Project, MarkerMode::Both}) {
                CHECK(MarkerSignature(trimmed, md) == MarkerSignature(plan, md));
                ItemRules tc;
                RecordApplied(tc, trimmed, md);  // committed trimmed
                CHECK(MarkersUpToDate(tc, plan, md));  // then extended
            }
        }
        {
            // A record committed before 10-4b (signature with project times, inside the item
            // only) still reads "up to date" while its item stays put.
            std::string text = MarkerModeWord(MarkerMode::Both);
            char        buf[160];
            for (const PlannedMarker& p : plan) {
                if (!p.in_item) continue;
                std::snprintf(buf, sizeof(buf), "\n%d|%08X|%.6f|%.6f|", p.block, static_cast<unsigned>(p.color), p.clip_t,
                              p.project_t);
                text += buf;
                text += p.name;
            }
            uint64_t h = 1469598103934665603ull;
            for (unsigned char ch : text) {
                h ^= ch;
                h *= 1099511628211ull;
            }
            std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(h));
            ItemRules legacy;
            legacy.has_applied = true;
            legacy.applied.mode = MarkerMode::Both;
            legacy.applied.sig = buf;
            CHECK(MarkersUpToDate(legacy, plan, MarkerMode::Both));
            ItemClipMap mv = m;
            mv.item_pos = 12.0;
            CHECK(!MarkersUpToDate(legacy, PlanMarkers(l, blocks, mv), MarkerMode::Both));  // as before 10-4b
        }
        // ---- 10-5 frozen signature ----
        // Captured from the 10-5 build; NEVER regenerate. Items committed by 10-5 store this
        // signature: a change to its input (format, fields, order) would show every one of
        // them as "out of date".
        {
            std::vector<PlannedMarker> fz(3);
            fz[0].clip_t = 0.4625;
            fz[0].project_t = 12.4625;
            fz[0].block = 0;
            fz[0].name = "Footstep L";
            fz[0].color = 0x1000000u | 0x5F9EDDu;
            fz[0].strength = 0.82;
            fz[0].speed = 1.91;
            fz[1].clip_t = 0.9875;
            fz[1].project_t = 12.9875;
            fz[1].block = 1;
            fz[1].name = "Footstep R";
            fz[1].color = 0x1000000u | 0xDD9E5Fu;
            fz[1].strength = 0.77;
            fz[1].speed = 1.64;
            fz[2].clip_t = 1.5125;
            fz[2].project_t = 13.5125;
            fz[2].block = 0;
            fz[2].name = "Footstep L";
            fz[2].color = 0x1000000u | 0x5F9EDDu;
            fz[2].user = true;
            CHECK(MarkerSignature(fz, MarkerMode::Both) == "9DAEAB29B5B1F277");
            CHECK(MarkerSignature(fz, MarkerMode::Take) == "A39AB38CBF9E787F");
        }
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
        CHECK(snap == 3 && r.events.size() == 6 && r.events[0].kind == EventKind::Detected);  // 2.5 included
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

    // ---- 10-4 fb-4: preview markers, Commit and Cancel -------------------------------------------
    {
        CHECK(PreviewMarkerName("Footstep L") == "Footstep L - Preview");
        // ~50 % toward black, rounded; no colour = a dark grey (still a colour).
        CHECK(PreviewColor(0x1000000u | 0x5F9EDDu) == (0x1000000u | 0x304F6Fu));
        CHECK(PreviewColor(0x1000000u | 0xFFFFFFu) == (0x1000000u | 0x808080u));
        CHECK(PreviewColor(0x1000000u | 0x000000u) == 0x1000000u);
        CHECK(PreviewColor(0) == (0x1000000u | 0x505050u));

        std::vector<Block> blocks = {Rule("Footstep L")};
        ItemClipMap m;
        m.item_pos = 10.0;
        m.item_len = 3.0;
        m.clip_len = 2.0;
        const std::vector<Event> det = {Det(0, 0.5), Det(0, 1.0)};
        const std::vector<PlannedMarker> plan = PlanMarkers(BuildEventList(det, {}, 1), blocks, m);
        CHECK(plan.size() == 2);
        const std::vector<PlannedMarker> pv = PreviewPlan(plan);
        CHECK(pv.size() == 2 && pv[0].name == "Footstep L - Preview" && pv[0].color == PreviewColor(plan[0].color) &&
              Near(pv[0].clip_t, plan[0].clip_t) && Near(pv[1].project_t, plan[1].project_t));

        // Never committed: previews as soon as there is a result; none for an empty one.
        ItemRules r;
        r.blocks = blocks;
        CHECK(PreviewNeeded(r, plan, MarkerMode::Both));
        CHECK(!PreviewNeeded(r, {}, MarkerMode::Both));
        // Committed: none while the result equals the committed one (same option).
        RecordApplied(r, plan, MarkerMode::Both);
        CHECK(!PreviewNeeded(r, plan, MarkerMode::Both));
        // A threshold change moved an event: previews.
        std::vector<PlannedMarker> moved = plan;
        moved[1].clip_t = 1.1;
        CHECK(PreviewNeeded(r, moved, MarkerMode::Both));
        CHECK(PreviewNeeded(r, {}, MarkerMode::Both));       // everything gone: the committed ones differ
        CHECK(PreviewNeeded(r, plan, MarkerMode::Take));     // another option
        // Back to the committed result: none again.
        CHECK(!PreviewNeeded(r, plan, MarkerMode::Both));

        // The preview state on the record.
        CHECK(!HasPreviews(r));
        RecordPreviewed(r, moved, MarkerMode::Both);
        TakeMarkerRef pt;
        pt.t = 1.1;
        pt.name = "Footstep L - Preview";
        r.ptmarkers = {pt};
        ProjectMarkerRef pp;
        pp.guid = "{P}";
        r.ppmarkers = {pp};
        CHECK(HasPreviews(r) && r.has_previewed && r.previewed.sig == MarkerSignature(moved, MarkerMode::Both));
        ItemRules back;
        CHECK(ParseItemRules(SerializeItemRules(r), &back) && back.ptmarkers.size() == 1 && back.ppmarkers.size() == 1 &&
              back.previewed.sig == r.previewed.sig && back.ptmarkers[0].name == "Footstep L - Preview");
        ItemRules cleared = r;
        ClearPreviewed(cleared);
        CHECK(!HasPreviews(cleared) && cleared.has_applied && cleared.tmarkers.size() == r.tmarkers.size());

        // Cancel: the snapshot's rules and events, the committed markers owned now, no preview.
        ItemRules snap;
        snap.blocks = blocks;
        snap.blocks[0].conditions.resize(1);
        snap.blocks[0].conditions[0].threshold = 0.05;
        snap.events = {MakeUserEvent(0, 0.7, 0, 0)};
        snap.has_applied = true;
        snap.applied.sig = "S";
        TakeMarkerRef old_tm;
        old_tm.name = "stale";
        snap.tmarkers = {old_tm};
        ItemRules cur = r;
        cur.blocks[0].conditions.resize(1);
        cur.blocks[0].conditions[0].threshold = 0.2;
        TakeMarkerRef tm;
        tm.t = 0.5;
        tm.name = "Footstep L";
        cur.tmarkers = {tm};
        ProjectMarkerRef snap_pm;
        snap_pm.guid = "{OLD}";
        snap.pmarkers = {snap_pm};
        ProjectMarkerRef cur_pm;
        cur_pm.guid = "{NOW}";
        cur_pm.t = 10.5;
        cur.pmarkers = {cur_pm};
        const ItemRules restored = RestoreCommitted(snap, cur);
        CHECK(restored.pmarkers.size() == 1 && restored.pmarkers[0].guid == "{NOW}" && restored.pmarkers[0].t == 10.5);
        CHECK(restored.blocks[0].conditions[0].threshold == 0.05);
        CHECK(restored.events.size() == 1 && restored.events[0].kind == EventKind::User);
        CHECK(restored.has_applied && restored.applied.sig == "S");
        CHECK(restored.tmarkers.size() == 1 && restored.tmarkers[0].name == "Footstep L");
        CHECK(!HasPreviews(restored));

        // The first snapshot: an item with a rule or a preset has rules.
        ItemRules none;
        CHECK(!HasRulesForSnapshot(none));
        none.has_preset = true;
        CHECK(HasRulesForSnapshot(none));
        CHECK(HasRulesForSnapshot(snap));

        // ---- CancelTarget ----
        const std::string snap_text = SerializeItemRules(snap);
        // 1. A snapshot: restored (the current committed markers kept, no preview).
        {
            ItemRules next;
            CHECK(CancelTarget(cur, SerializeItemRules(cur), &snap_text, &next));
            CHECK(next.blocks.size() == 1 && next.blocks[0].conditions[0].threshold == 0.05);
            CHECK(next.tmarkers.size() == 1 && next.tmarkers[0].name == "Footstep L" && !HasPreviews(next));
            CHECK(next.pmarkers.size() == 1 && next.pmarkers[0].guid == "{NOW}");
        }
        // 1b. Every rule deleted after a commit: the snapshot brings them back.
        {
            ItemRules empty = cur;
            empty.blocks.clear();
            ClearPreviewed(empty);
            ItemRules next;
            CHECK(CancelTarget(empty, SerializeItemRules(empty), &snap_text, &next));
            CHECK(next.blocks.size() == 1 && next.blocks[0].conditions[0].threshold == 0.05);
        }
        // 2. No snapshot (or one that does not read): only the previews go.
        {
            ItemRules next;
            CHECK(CancelTarget(cur, SerializeItemRules(cur), nullptr, &next));
            CHECK(!HasPreviews(next) && next.blocks[0].conditions[0].threshold == 0.2);
            const std::string bad = "not a record";
            ItemRules next2;
            CHECK(CancelTarget(cur, SerializeItemRules(cur), &bad, &next2) && !HasPreviews(next2) &&
                  next2.blocks[0].conditions[0].threshold == 0.2);
        }
        // 3. Nothing to cancel: no previews and the record already is the target.
        {
            ItemRules same = RestoreCommitted(snap, cur);
            ItemRules next;
            next.format_version = 99;
            CHECK(!CancelTarget(same, SerializeItemRules(same), &snap_text, &next));
            CHECK(next.format_version == 99);  // untouched
            ItemRules plain = cur;
            ClearPreviewed(plain);
            CHECK(!CancelTarget(plain, SerializeItemRules(plain), nullptr, &next));
        }

        // ---- FirstCommittedSnapshot ----
        {
            ItemRules empty_rec;
            const std::string braw = "RAVRULES 1\nbefore\n";
            CHECK(FirstCommittedSnapshot(true, true, snap, braw, cur).empty());   // one already there
            CHECK(FirstCommittedSnapshot(false, true, snap, braw, cur) == braw);  // had rules: before
            CHECK(FirstCommittedSnapshot(false, false, snap, braw, cur) == SerializeItemRules(cur));  // unreadable
            CHECK(FirstCommittedSnapshot(false, true, empty_rec, braw, cur) == SerializeItemRules(cur));  // first rules
            CHECK(FirstCommittedSnapshot(false, true, empty_rec, braw, empty_rec).empty());  // still no rules
        }

        // ---- ComposeCommittedRecord ----
        {
            ItemRules rec = cur;  // committed markers + previews (take + project)
            CHECK(HasPreviews(rec));
            TakeMarkerRef nt;
            nt.t = 1.1;
            nt.name = "Footstep L";
            ProjectMarkerRef np;
            np.guid = "{NEW}";
            ItemRules a = rec;
            ComposeCommittedRecord(a, {nt}, {np}, false, false, moved, MarkerMode::Both);
            CHECK(!HasPreviews(a) && a.ptmarkers.empty() && a.ppmarkers.empty());
            CHECK(a.tmarkers.size() == 1 && a.tmarkers[0].t == 1.1 && a.pmarkers.size() == 1 && a.pmarkers[0].guid == "{NEW}");
            CHECK(a.has_applied && a.applied.mode == MarkerMode::Both && a.applied.sig == MarkerSignature(moved, MarkerMode::Both));
            CHECK(MarkersUpToDate(a, moved, MarkerMode::Both));
            // A kind that could not be deleted (its API missing): its committed and preview refs stay RAV's.
            ItemRules b = rec;
            ComposeCommittedRecord(b, {nt}, {}, false, true, moved, MarkerMode::Take);
            CHECK(!b.has_previewed && b.ptmarkers.empty());
            CHECK(b.ppmarkers.size() == rec.ppmarkers.size() && b.ppmarkers[0].guid == "{P}");
            CHECK(b.pmarkers.size() == 1 && b.pmarkers[0].guid == "{NOW}");  // kept, nothing appended
            CHECK(b.applied.mode == MarkerMode::Take && b.applied.sig == MarkerSignature(moved, MarkerMode::Take));
            ItemRules c = rec;
            ComposeCommittedRecord(c, {nt}, {np}, true, false, moved, MarkerMode::Both);
            CHECK(c.ptmarkers.size() == 1 && c.ppmarkers.empty() && c.tmarkers.size() == 2);
        }
    }


    // ---- 10-4b: the marker mirror ---------------------------------------------------------------
    {
        auto ref = [](const char* guid, double c, double t) {
            ProjectMarkerRef r;
            r.guid = guid;
            r.c = c;
            r.t = t;
            r.name = "Step";
            r.color = 0x1000000u | 0x5F9EDDu;
            r.has_c = r.has_color = r.has_name = true;
            return r;
        };
        auto at = [](double t) {
            MirrorRefState s;
            s.exists = true;
            s.now_t = t;
            return s;
        };
        ItemClipMap m;
        m.item_pos = 10.0;
        m.item_len = 3.0;
        m.clip_len = 2.0;
        ProjectMarkerRef old_ref;  // an older record's ref: no clip time
        old_ref.guid = "{O}";
        old_ref.t = 10.9;
        const std::vector<ProjectMarkerRef> refs = {ref("{A}", 0.5, 10.5), ref("{B}", 1.5, 11.5), old_ref};
        const std::vector<MirrorRefState>   placed = {at(10.5), at(11.5), at(10.9)};

        // Move: 2 s right, its markers follow; the older ref stays.
        ItemClipMap mv = m;
        mv.item_pos = 12.0;
        std::vector<MirrorStep> st = MirrorPlan(refs, placed, mv, false, false);
        CHECK(st.size() == 3 && st[0].action == MirrorAction::Move && Near(st[0].new_t, 12.5));
        CHECK(st[1].action == MirrorAction::Move && Near(st[1].new_t, 13.5) && st[2].action == MirrorAction::Keep);
        // Nothing changed: nothing to do.
        for (const MirrorStep& x : MirrorPlan(refs, placed, m, false, false)) CHECK(x.action == MirrorAction::Keep);
        // Already in place after a change (e.g. an undo restored both): nothing to do.
        for (const MirrorStep& x : MirrorPlan(refs, placed, m, false, false)) CHECK(x.action == MirrorAction::Keep);
        // Rate 2x: pos + (c - offs) / rate.
        ItemClipMap rt = m;
        rt.rate = 2.0;
        st = MirrorPlan(refs, placed, rt, false, false);
        CHECK(st[0].action == MirrorAction::Move && Near(st[0].new_t, 10.25) && Near(st[1].new_t, 10.75));
        // Start offset (left trim) 0.25 s, rate 2x: 10 + (1.5 - 0.25) / 2.
        rt.start_offs = 0.25;
        st = MirrorPlan(refs, placed, rt, false, false);
        CHECK(st[0].action == MirrorAction::Move && Near(st[0].new_t, 10.125) && Near(st[1].new_t, 10.625));

        // Trim the end before B: B is hidden (its marker deleted), A stays.
        ItemClipMap tr = m;
        tr.item_len = 1.0;
        st = MirrorPlan(refs, placed, tr, false, false);
        CHECK(st[0].action == MirrorAction::Keep && st[1].action == MirrorAction::Hide && st[1].delete_old);
        // Extend again: the hidden B comes back at the right time.
        std::vector<ProjectMarkerRef> hidden = refs;
        hidden[1].guid.clear();
        std::vector<MirrorRefState> hs = placed;
        hs[1] = MirrorRefState{};
        st = MirrorPlan(hidden, hs, m, false, false);
        CHECK(st[1].action == MirrorAction::Show && Near(st[1].new_t, 11.5) && st[0].action == MirrorAction::Keep);
        // Still trimmed: it stays hidden.
        CHECK(MirrorPlan(hidden, hs, tr, false, false)[1].action == MirrorAction::Keep);

        // 10-4c replaces 10-4b's interim rule (a dragged / deleted marker left alone): see the
        // 10-4c section below. A deleted one is a user delete.
        std::vector<MirrorRefState> gone = placed;
        gone[0] = MirrorRefState{};
        CHECK(MirrorPlan(refs, gone, m, false, false)[0].action == MirrorAction::UserDelete);
        CHECK(MirrorPlan(refs, gone, m, false, false)[2].action == MirrorAction::Keep);  // older ref
        // One the mirror deleted (its item deleted, then restored by Ctrl+Z): back.
        gone[0].restore = true;
        st = MirrorPlan(refs, gone, m, false, false);
        CHECK(st[0].action == MirrorAction::Show && Near(st[0].new_t, 10.5));
        st = MirrorPlan(refs, gone, tr, false, false);
        CHECK(st[0].action == MirrorAction::Show);  // A is inside the trimmed item too
        gone[1] = MirrorRefState{};
        gone[1].restore = true;
        st = MirrorPlan(refs, gone, tr, false, false);
        CHECK(st[1].action == MirrorAction::Hide && !st[1].delete_old);  // outside: kept hidden

        // Split at 11.0: the left part (the original item) keeps A, hides B; the right part (a
        // copy) gets its own B and never touches the original's A. No marker twice.
        ItemClipMap left = m, right = m;
        left.item_len = 1.0;
        right.item_pos = 11.0;
        right.item_len = 2.0;
        right.start_offs = 1.0;
        const std::vector<MirrorStep> ls = MirrorPlan(refs, placed, left, false, false);
        const std::vector<MirrorStep> rs = MirrorPlan(refs, placed, right, false, true);
        CHECK(ls[0].action == MirrorAction::Keep && ls[1].action == MirrorAction::Hide && ls[1].delete_old);
        CHECK(rs[0].action == MirrorAction::Hide && !rs[0].delete_old);
        CHECK(rs[1].action == MirrorAction::Show && Near(rs[1].new_t, 11.5));
        CHECK(rs[2].action == MirrorAction::Hide && !rs[2].delete_old);  // an older ref's marker is the original's
        int shown_b = 0;
        shown_b += (ls[1].action == MirrorAction::Keep || ls[1].action == MirrorAction::Move) ? 1 : 0;
        shown_b += (rs[1].action == MirrorAction::Show) ? 1 : 0;
        CHECK(shown_b == 1);

        // Duplicate at 20 s: the copy gets its own markers; the original's stay.
        ItemClipMap dup = m;
        dup.item_pos = 20.0;
        st = MirrorPlan(refs, placed, dup, false, true);
        CHECK(st[0].action == MirrorAction::Show && Near(st[0].new_t, 20.5));
        CHECK(st[1].action == MirrorAction::Show && Near(st[1].new_t, 21.5));
        CHECK(st[2].action == MirrorAction::Hide && !st[2].delete_old);  // the copy does not share the older marker
        CHECK(MirrorPlan(refs, placed, m, false, false)[0].action == MirrorAction::Keep);  // the original

        // What Commit writes / records per planned marker, on a trimmed item.
        {
            std::vector<Block> wb = {Rule("Step")};
            ItemClipMap        wm;
            wm.item_pos = 10.0;
            wm.item_len = 1.0;  // trimmed: clip 1.0 .. 2.0 is outside
            wm.clip_len = 2.0;
            const std::vector<PlannedMarker> wp =
                PlanMarkers(BuildEventList({Det(0, 0.5), Det(0, 1.5), Det(0, 1.5)}, {}, 1), wb, wm);
            CHECK(wp.size() == 3);
            const std::vector<MarkerWrite> both = PlanMarkerWrites(wp, true, true);
            CHECK(both.size() == 3);
            if (both.size() == 3) {
                CHECK(both[0].take && both[0].project && !both[0].hidden && both[0].ref.guid.empty());
                CHECK(both[0].ref.has_c && Near(both[0].ref.c, 0.5) && both[0].ref.has_color &&
                      both[0].ref.color == (0x1000000u | 0x5F9EDDu) && both[0].ref.has_name && both[0].ref.name == "Step");
                // Outside: a take marker (source time) + a hidden project ref, the ref once per twin.
                CHECK(both[1].take && !both[1].project && both[1].hidden && Near(both[1].ref.c, 1.5) &&
                      both[1].ref.guid.empty() && both[1].ref.name == "Step");
                CHECK(both[2].take && !both[2].project && !both[2].hidden);
            }
            const std::vector<MarkerWrite> tk = PlanMarkerWrites(wp, true, false);
            CHECK(tk.size() == 3 && tk[0].take && tk[1].take && !tk[1].project && !tk[1].hidden);
            const std::vector<MarkerWrite> pj = PlanMarkerWrites(wp, false, true);
            CHECK(pj.size() == 2 && !pj[0].take && pj[0].project && pj[1].hidden && !pj[1].take);

            // Never committed, every event outside the item: previews are still needed (Commit
            // records the hidden refs).
            ItemClipMap out = wm;
            out.item_len = 0.25;
            const std::vector<PlannedMarker> op = PlanMarkers(BuildEventList({Det(0, 0.5)}, {}, 1), wb, out);
            CHECK(op.size() == 1 && !op[0].in_item);
            ItemRules nc;
            nc.blocks = wb;
            CHECK(PreviewNeeded(nc, op, MarkerMode::Project));
        }

        // Ownership arbitration.
        {
            auto in = [](const char* item, const char* owner, std::vector<std::string> g) {
                MirrorOwnerInput i;
                i.item = item;
                i.owner = owner;
                i.managed = true;
                i.guids = std::move(g);
                return i;
            };
            // Original + duplicate (the copy's record names the original): the copy loses.
            MirrorOwnerResult r1 = ArbitrateMirrorOwnership({in("{I1}", "{I1}", {"{A}"}), in("{I2}", "{I1}", {"{A}"})},
                                                            {{"{A}", "{I1}"}}, {"{I1}", "{I2}"});
            CHECK(r1.own[0] == MirrorOwnership::Own && r1.own[1] == MirrorOwnership::Copy && r1.claimed["{A}"] == "{I1}");
            // Two self-owners of one marker, the last scan had it at I1: I2 is the copy (in any order).
            MirrorOwnerResult r2 = ArbitrateMirrorOwnership({in("{I2}", "{I2}", {"{A}"}), in("{I1}", "{I1}", {"{A}"})},
                                                            {{"{A}", "{I1}"}}, {"{I1}", "{I2}"});
            CHECK(r2.own[0] == MirrorOwnership::Copy && r2.own[1] == MirrorOwnership::Own && r2.claimed["{A}"] == "{I1}");
            // ... on a first scan (no last scan): the first claimant keeps it.
            MirrorOwnerResult r3 = ArbitrateMirrorOwnership({in("{I2}", "{I2}", {"{A}"}), in("{I1}", "{I1}", {"{A}"})},
                                                            {}, {"{I1}", "{I2}"});
            CHECK(r3.own[0] == MirrorOwnership::Own && r3.own[1] == MirrorOwnership::Copy && r3.claimed["{A}"] == "{I2}");
            // A record without owner whose marker the last scan gave to an item still present: a copy.
            MirrorOwnerResult r4 = ArbitrateMirrorOwnership({in("{I2}", "", {"{A}"})}, {{"{A}", "{I1}"}}, {"{I1}", "{I2}"});
            CHECK(r4.own[0] == MirrorOwnership::Copy && r4.claimed.empty());
            // ... its holder deleted: it adopts.
            MirrorOwnerResult r5 = ArbitrateMirrorOwnership({in("{I2}", "", {"{A}"})}, {{"{A}", "{I1}"}}, {"{I2}"});
            CHECK(r5.own[0] == MirrorOwnership::Adopt && r5.claimed["{A}"] == "{I2}");
            // Unmanaged items are left out.
            MirrorOwnerInput um = in("{I3}", "", {"{A}"});
            um.managed = false;
            MirrorOwnerResult r6 = ArbitrateMirrorOwnership({um}, {}, {"{I3}"});
            CHECK(r6.own[0] == MirrorOwnership::Own && r6.claimed.empty());

            // After the scan: a copy that could not refresh holds nothing (still the original's GUIDs).
            const std::vector<MirrorHeld> held = {{"{I1}", true, true, {"{A}"}}, {"{I2}", true, false, {"{A}"}},
                                                  {"{I4}", true, true, {"{N}"}}};
            const std::map<std::string, std::string> nx =
                NextMirrorOwners(held, {{"{A}", "{I1}"}, {"{K}", "{I5}"}, {"{Z}", "{I9}"}}, {"{I1}", "{I2}", "{I4}", "{I5}"},
                                 {"{I1}", "{I2}", "{I4}"});
            CHECK(nx.size() == 3 && nx.at("{A}") == "{I1}" && nx.at("{N}") == "{I4}" && nx.at("{K}") == "{I5}");
            CHECK(MirrorClaimedNow(held) == (std::vector<std::string>{"{A}", "{N}"}));
        }

        // Ownership.
        CHECK(DecideMirrorOwnership("{I1}", "{I1}", false) == MirrorOwnership::Own);
        CHECK(DecideMirrorOwnership("{I1}", "{I2}", false) == MirrorOwnership::Copy);  // duplicate / paste / split
        CHECK(DecideMirrorOwnership("", "{I1}", false) == MirrorOwnership::Adopt);
        CHECK(DecideMirrorOwnership("", "{I2}", true) == MirrorOwnership::Copy);
        ItemRules rec;
        CHECK(RecordOwner(rec).empty() && !SetRecordOwner(rec, "{I1}"));  // nowhere to keep it
        rec.has_previewed = true;
        CHECK(SetRecordOwner(rec, "{I1}") && RecordOwner(rec) == "{I1}");
        rec.has_applied = true;
        rec.applied.item = "{I0}";
        CHECK(RecordOwner(rec) == "{I0}");  // the applied line wins
        CHECK(SetRecordOwner(rec, "{I2}") && rec.applied.item == "{I2}" && rec.previewed.item == "{I2}");

        // Old record: refs without clip time are not the mirror's; with one they are.
        ItemRules o;
        o.pmarkers = {old_ref};
        CHECK(!MirrorManaged(o));
        o.ppmarkers = {refs[0]};
        CHECK(MirrorManaged(o));

        // Deleted item: its markers go; an item present but not read keeps them; a marker still
        // listed stays.
        const std::vector<std::pair<std::string, std::string>> prev = {
            {"{A}", "{I1}"}, {"{B}", "{I1}"}, {"{C}", "{I2}"}, {"{D}", "{I3}"}, {"{E}", "{I4}"}};
        const std::vector<std::string> orph =
            MirrorOrphans(prev, {"{A}"}, {"{I1}", "{I3}", "{I4}"}, {"{I1}", "{I4}"});
        // B: I1 read, no longer lists it; C: I2 deleted; D: I3 present, not read (kept);
        // E: I4 read, no longer lists it.
        CHECK(orph == (std::vector<std::string>{"{B}", "{C}", "{E}"}));
        CHECK(MirrorOrphans({}, {}, {}, {}).empty());

        // Cancel keeps the current item as owner (a copy's snapshot names the original).
        ItemRules snapr;
        snapr.has_applied = true;
        snapr.applied.item = "{ORIG}";
        ItemRules curr = snapr;
        curr.applied.item = "{COPY}";
        CHECK(RestoreCommitted(snapr, curr).applied.item == "{COPY}");
    }

    // ---- 10-4c ----
    // REAPER-side edits of RAV's markers -> the event list (MirrorPlan's user steps,
    // ApplyMarkerEdits, MatchTakeRefs).
    {
        const uint32_t kCol = 0x1000000u | 0x5F9EDDu;
        auto ref = [&](const char* guid, double c, double t) {
            ProjectMarkerRef r;
            r.guid = guid;
            r.c = c;
            r.t = t;
            r.name = "Step";
            r.color = kCol;
            r.has_c = r.has_color = r.has_name = true;
            return r;
        };
        auto at = [](double t, const char* name = "Step") {
            MirrorRefState s;
            s.exists = true;
            s.now_t = t;
            s.name = name;
            s.has_name = true;
            return s;
        };
        auto edit = [&](MarkerEditKind k, double c, double new_c = 0.0, bool preview = false) {
            MarkerEdit e;
            e.kind = k;
            e.c = c;
            e.new_c = new_c;
            e.name = preview ? PreviewMarkerName("Step") : "Step";
            e.color = preview ? PreviewColor(kCol) : kCol;
            e.has_color = true;
            e.preview = preview;
            return e;
        };
        ItemClipMap m;
        m.item_pos = 10.0;
        m.item_len = 3.0;
        m.clip_len = 2.0;
        const std::vector<ProjectMarkerRef> refs = {ref("{A}", 0.5, 10.5), ref("{B}", 1.5, 11.5)};
        const std::vector<MirrorRefState>   placed = {at(10.5), at(11.5)};

        // A committed item, up to date: detections at 0.5 and 1.5 (the applied snapshot keeps
        // their values), a Commit in Project mode.
        const std::vector<Event> det = {Det(0, 0.5, 0.8), Det(0, 1.5, 0.9)};
        ItemRules rec;
        rec.blocks = {Rule("Step", kCol)};
        rec.pmarkers = refs;
        auto plan_of = [&](const ItemRules& r) {
            return PlanMarkers(BuildEventList(det, r.events, r.blocks.size()), r.blocks, m);
        };
        RecordApplied(rec, plan_of(rec), MarkerMode::Project);
        rec.applied.item = "{I1}";
        CHECK(MarkersUpToDate(rec, plan_of(rec), MarkerMode::Project));
        CHECK(rec.events.size() == 2 && Near(rec.events[0].strength, 0.8));

        // Drag detected: A dragged 100 ms right, item untouched -> a user drag to clip 0.6.
        std::vector<MirrorRefState> dragged = placed;
        dragged[0].now_t = 10.6;
        std::vector<MirrorStep> st = MirrorPlan(refs, dragged, m, false, false);
        CHECK(st[0].action == MirrorAction::UserDrag && Near(st[0].new_c, 0.6, 1e-9) && Near(st[0].new_t, 10.6));
        CHECK(st[1].action == MirrorAction::Keep);
        {
            std::vector<int> bl;
            ItemRules after = ApplyMarkerEdits(rec, {edit(MarkerEditKind::Drag, 0.5, st[0].new_c)}, &det, &bl);
            CHECK(bl.size() == 1 && bl[0] == 0);
            // Suppression at the old time + user event at the new one, the detection's values kept.
            const std::vector<ShownEvent> l = BuildEventList(det, after.events, 1);
            CHECK(Count(l, ShownKind::Suppressed) == 1 && Count(l, ShownKind::User) == 1 && Count(l, ShownKind::Detected) == 1);
            for (const ShownEvent& e : l)
                if (e.kind == ShownKind::User) CHECK(Near(e.t, 0.6, 1e-9) && Near(e.strength, 0.8) && Near(e.speed, 2.0));
            // The commit's signature follows (the item was up to date): "markers up to date".
            after.applied.sig = MarkerSignature(plan_of(after), MarkerMode::Project);
            CHECK(MarkersUpToDate(after, plan_of(after), MarkerMode::Project));
            CHECK(!PreviewNeeded(after, plan_of(after), MarkerMode::Project));
            // The ref now says where the marker is: the next tick finds it in place.
            std::vector<ProjectMarkerRef> r2 = refs;
            r2[0].c = st[0].new_c;
            r2[0].t = st[0].new_t;
            for (const MirrorStep& x : MirrorPlan(r2, dragged, m, false, false)) CHECK(x.action == MirrorAction::Keep);
            // Undo: marker and record back together -> nothing to do; redo (marker dragged,
            // record not yet) -> the same edit again.
            for (const MirrorStep& x : MirrorPlan(refs, placed, m, false, false)) CHECK(x.action == MirrorAction::Keep);
            CHECK(MirrorPlan(refs, dragged, m, false, false)[0].action == MirrorAction::UserDrag);
        }
        // Drag user event: that user event moves, its values kept, nothing suppressed.
        {
            ItemRules u = rec;
            u.events.push_back(MakeUserEvent(0, 1.2, 0.3, 0.4));
            ItemRules after = ApplyMarkerEdits(u, {edit(MarkerEditKind::Drag, 1.2, 1.25)}, &det);
            CHECK(after.events.size() == u.events.size());
            CHECK(Near(after.events.back().t, 1.25) && Near(after.events.back().strength, 0.3) &&
                  after.events.back().kind == EventKind::User);
            // Delete user event: removed.
            ItemRules del = ApplyMarkerEdits(u, {edit(MarkerEditKind::Delete, 1.2)});
            CHECK(del.events.size() == rec.events.size());
            CHECK(Count(BuildEventList(det, del.events, 1), ShownKind::User) == 0);
        }
        // Delete detected: suppressed (restorable in the view), once.
        {
            ItemRules after = ApplyMarkerEdits(rec, {edit(MarkerEditKind::Delete, 1.5)});
            const std::vector<ShownEvent> l = BuildEventList(det, after.events, 1);
            CHECK(Count(l, ShownKind::Suppressed) == 1 && Count(l, ShownKind::Detected) == 1);
            ItemRules twice = ApplyMarkerEdits(after, {edit(MarkerEditKind::Delete, 1.5)});
            CHECK(twice.events.size() == after.events.size());  // no second suppression
            // The marker missing on the timeline (not deleted by the mirror) = a user delete.
            std::vector<MirrorRefState> gone = placed;
            gone[1] = MirrorRefState{};
            CHECK(MirrorPlan(refs, gone, m, false, false)[1].action == MirrorAction::UserDelete);
        }
        // Rename: the marker becomes the user's, the event is suppressed.
        {
            std::vector<MirrorRefState> ren = placed;
            ren[0].name = "Step (mine)";
            st = MirrorPlan(refs, ren, m, false, false);
            CHECK(st[0].action == MirrorAction::UserDisown && st[1].action == MirrorAction::Keep);
            ItemRules after = ApplyMarkerEdits(rec, {edit(MarkerEditKind::Disown, 0.5)});
            CHECK(Count(BuildEventList(det, after.events, 1), ShownKind::Suppressed) == 1);
            // A name that cannot be read is never a rename.
            ren[0].has_name = false;
            CHECK(MirrorPlan(refs, ren, m, false, false)[0].action == MirrorAction::Keep);
        }
        // Outside item: dragged past the item's end -> the user's, event suppressed.
        {
            std::vector<MirrorRefState> out = placed;
            out[1].now_t = 13.5;
            CHECK(MirrorPlan(refs, out, m, false, false)[1].action == MirrorAction::UserDisown);
        }
        // Preview: the event list changes like a committed marker's; the commit is untouched.
        {
            ItemRules pv = rec;
            ItemRules after = ApplyMarkerEdits(pv, {edit(MarkerEditKind::Drag, 0.5, 0.7, true)}, &det);
            CHECK(Count(BuildEventList(det, after.events, 1), ShownKind::User) == 1);
            // ApplyMarkerEdits changes the events only: the record's other fields (signature, refs,
            // owner) are left alone (the commit staying untouched is tag_markers.cpp's job).
            CHECK(after.applied.sig == rec.applied.sig && after.pmarkers.size() == rec.pmarkers.size() &&
                  after.applied.item == "{I1}");
            // A preview edit resolves its rule by the preview name and darkened colour.
            CHECK(MarkerEditBlock(pv, edit(MarkerEditKind::Delete, 0.5, 0.0, true)) == 0);
            CHECK(MarkerEditBlock(pv, edit(MarkerEditKind::Delete, 0.5, 0.0, false)) == 0);
        }
        // Rule resolution: same name, the colour decides; an unknown name changes nothing;
        // the same event twice is edited once.
        {
            ItemRules two = rec;
            two.blocks = {Rule("Step", 0x1000000u | 0xFF0000u), Rule("Step", kCol)};
            CHECK(MarkerEditBlock(two, edit(MarkerEditKind::Delete, 0.5)) == 1);
            MarkerEdit unk = edit(MarkerEditKind::Delete, 0.5);
            unk.name = "Other";
            std::vector<int> bl;
            ItemRules same = ApplyMarkerEdits(rec, {unk}, nullptr, &bl);
            CHECK(bl.size() == 1 && bl[0] == -1 && same.events.size() == rec.events.size());
            ItemRules once = ApplyMarkerEdits(rec, {edit(MarkerEditKind::Drag, 0.5, 0.6), edit(MarkerEditKind::Drag, 0.5, 0.7)},
                                              nullptr, &bl);
            CHECK(bl[0] == 0 && bl[1] == -1 && once.events.size() == rec.events.size() + 2);
            // No snapshot entry: the live detection's values (within 1 ms).
            ItemRules nos = rec;
            nos.events.clear();
            ItemRules d2 = ApplyMarkerEdits(nos, {edit(MarkerEditKind::Drag, 1.5, 1.6)}, &det);
            CHECK(d2.events.size() == 2 && Near(d2.events[1].strength, 0.9));
        }
        // Tie-break: two rules with the same name and colour -> the one with an event at the clip time.
        {
            ItemRules tie;
            tie.blocks = {Rule("Step", kCol), Rule("Step", kCol)};
            EventEntry snap1;  // only rule 1 has a Detected snapshot entry at 1.5
            snap1.t = 1.5;
            snap1.kind = EventKind::Detected;
            snap1.block = 1;
            snap1.strength = 0.6;
            snap1.speed = 0.7;
            snap1.has_strength = snap1.has_speed = true;
            tie.events = {snap1};
            CHECK(MarkerEditBlock(tie, edit(MarkerEditKind::Delete, 1.5)) == 1);
            ItemRules sup = ApplyMarkerEdits(tie, {edit(MarkerEditKind::Delete, 1.5)});
            CHECK(sup.events.size() == 2 && sup.events[1].kind == EventKind::Suppress && sup.events[1].block == 1);
            ItemRules mvd = ApplyMarkerEdits(tie, {edit(MarkerEditKind::Drag, 1.5, 1.6)});
            CHECK(mvd.events.size() == 3 && mvd.events[2].kind == EventKind::User && mvd.events[2].block == 1 &&
                  Near(mvd.events[2].strength, 0.6));
            // User-entry variant: only rule 1 has a user event at 0.9 -> it moves, on rule 1.
            ItemRules tu;
            tu.blocks = tie.blocks;
            tu.events = {MakeUserEvent(1, 0.9, 0.2, 0.3)};
            CHECK(MarkerEditBlock(tu, edit(MarkerEditKind::Drag, 0.9, 1.0)) == 1);
            ItemRules um = ApplyMarkerEdits(tu, {edit(MarkerEditKind::Drag, 0.9, 1.0)});
            CHECK(um.events.size() == 1 && um.events[0].block == 1 && Near(um.events[0].t, 1.0) &&
                  um.events[0].kind == EventKind::User);
            // Neither has an event there: the first.
            CHECK(MarkerEditBlock(tu, edit(MarkerEditKind::Delete, 0.4)) == 0);
        }
        // The item changed too (tempo / timebase change, a ripple moving the marker by another
        // amount): the item wins, never a user drag.
        {
            ItemClipMap mv = m;
            mv.item_pos = 12.0;
            std::vector<MirrorRefState> odd = {at(12.7), at(11.9)};  // at neither place
            st = MirrorPlan(refs, odd, mv, true, false);
            CHECK(st[0].action == MirrorAction::Move && Near(st[0].new_t, 12.5));
            CHECK(st[1].action == MirrorAction::Move && Near(st[1].new_t, 13.5));
            CHECK(MirrorPlan(refs, odd, mv, false, false)[0].action == MirrorAction::UserDrag);  // item unchanged: a user drag
            ItemClipMap tr = m;
            tr.item_len = 1.0;  // B now outside, its marker at neither place
            std::vector<MirrorRefState> odd2 = {at(10.5), at(11.7)};
            st = MirrorPlan(refs, odd2, tr, true, false);
            CHECK(st[1].action == MirrorAction::Hide && st[1].delete_old);
            // A rename still reads as a rename.
            odd[0].name = "mine";
            CHECK(MirrorPlan(refs, odd, mv, true, false)[0].action == MirrorAction::UserDisown);
        }
        // A missing marker whose event is now outside the item: hidden, not a user delete.
        {
            ItemClipMap tr = m;
            tr.item_len = 1.0;
            std::vector<MirrorRefState> gone = placed;
            gone[1] = MirrorRefState{};
            st = MirrorPlan(refs, gone, tr, false, false);
            CHECK(st[1].action == MirrorAction::Hide && !st[1].delete_old);
            CHECK(MirrorPlan(refs, gone, m, false, false)[1].action == MirrorAction::UserDelete);  // inside: a delete
        }
        // Item moved vs drag vs ripple.
        {
            ItemClipMap mv = m;
            mv.item_pos = 12.0;
            st = MirrorPlan(refs, placed, mv, false, false);  // item moved, markers untouched: they follow
            CHECK(st[0].action == MirrorAction::Move && Near(st[0].new_t, 12.5));
            std::vector<MirrorRefState> rip = {at(12.5), at(13.5)};  // ripple: moved with the item
            st = MirrorPlan(refs, rip, mv, false, false);
            CHECK(st[0].action == MirrorAction::Retime && Near(st[0].new_t, 12.5) && st[1].action == MirrorAction::Retime);
            std::vector<MirrorRefState> drag = placed;  // item untouched, marker dragged
            drag[0].now_t = 10.4;
            CHECK(MirrorPlan(refs, drag, m, false, false)[0].action == MirrorAction::UserDrag);
        }
        // First sight after reopen: markers where the record says -> nothing moves, nothing changes.
        for (const MirrorStep& x : MirrorPlan(refs, placed, m, false, false)) CHECK(x.action == MirrorAction::Keep);
        // Old record: refs without clip time (or without name) -> edits ignored, as 10-4b.
        {
            ProjectMarkerRef o;
            o.guid = "{O}";
            o.t = 10.9;
            ProjectMarkerRef nn = ref("{N}", 1.0, 11.0);
            nn.has_name = false;
            std::vector<MirrorRefState> ds = {at(10.7), at(11.3)};
            st = MirrorPlan({o, nn}, ds, m, false, false);
            CHECK(st[0].action == MirrorAction::Keep && st[1].action == MirrorAction::Keep);
            st = MirrorPlan({o, nn}, {MirrorRefState{}, MirrorRefState{}}, m, false, false);
            CHECK(st[0].action == MirrorAction::Keep && st[1].action == MirrorAction::Keep);
        }
        // A copy never reads edits.
        {
            std::vector<MirrorRefState> dragged2 = placed;
            dragged2[0].now_t = 10.6;
            CHECK(MirrorPlan(refs, dragged2, m, false, true)[0].action == MirrorAction::Show);
        }
        // Take markers: matched by time and name, then by elimination.
        {
            TakeMarkerRef a, b, c, d;
            a.t = 0.5;
            b.t = 1.5;
            c.t = 1.0;
            d.t = 1.8;
            a.name = b.name = c.name = d.name = "Step";
            const std::vector<ExistingMarker> mk = {
                {0.5, "Step"},       // a in place
                {1.5, "Mine"},       // b renamed
                {0.7, "Step"},       // c dragged here (nearest unowned same name)
                {0.5000001, "Step"}, // a foreign twin of a: used as a candidate for c? farther than 0.7
            };
            const std::vector<TakeRefMatch> tm = MatchTakeRefs({a, b, c, d}, mk, 1e-5);
            CHECK(tm.size() == 4);
            CHECK(tm[0].fate == TakeRefFate::InPlace && tm[0].marker == 0);
            CHECK(tm[1].fate == TakeRefFate::Rename && tm[1].marker == 1);
            CHECK(tm[2].fate == TakeRefFate::Drag && tm[2].marker == 2);
            // d: the only unowned same-name marker left is the far one at 0.5000001: a drag there.
            CHECK(tm[3].fate == TakeRefFate::Drag && tm[3].marker == 3);
            const std::vector<TakeRefMatch> none = MatchTakeRefs({a, c}, {{0.5, "Step"}}, 1e-5);
            CHECK(none[0].fate == TakeRefFate::InPlace && none[1].fate == TakeRefFate::Delete && none[1].marker == -1);
            // Every marker in place: nothing to do.
            for (const TakeRefMatch& x : MatchTakeRefs({a, b}, {{1.5, "Step"}, {0.5, "Step"}}, 1e-5))
                CHECK(x.fate == TakeRefFate::InPlace);
            // A take marker's edit uses its source time as the event's clip time.
            ItemRules after = ApplyMarkerEdits(rec, {edit(MarkerEditKind::Drag, a.t, 0.7)}, &det);
            CHECK(Count(BuildEventList(det, after.events, 1), ShownKind::User) == 1);
        }
        // Both: the twin of an edited marker (same name, same clip time), either way.
        {
            TakeMarkerRef t0, t1, t2;
            t0.t = 0.5;
            t0.name = "Other";  // another rule's name at the same time: not a twin
            t1.t = 0.5;
            t1.name = "Step";
            t2.t = 1.5;
            t2.name = "Step";
            const std::vector<TakeMarkerRef> tk = {t0, t1, t2};
            // A project-marker edit finds the committed take ref of its event.
            CHECK(FindTakeTwin(tk, refs[0].c, refs[0].name, 1e-5, {}) == 1);
            CHECK(FindTakeTwin(tk, refs[1].c, refs[1].name, 1e-5, {}) == 2);
            CHECK(FindTakeTwin(tk, 1.0, "Step", 1e-5, {}) == -1);   // another time
            CHECK(FindTakeTwin(tk, 0.5, "Steps", 1e-5, {}) == -1);  // another name
            CHECK(FindTakeTwin(tk, 0.50002, "Step", 1e-5, {}) == -1);  // outside the tolerance
            CHECK(FindTakeTwin(tk, 0.500005, "Step", 1e-5, {}) == 1);  // inside it
            // Already handled (or dropped) this scan: skipped.
            CHECK(FindTakeTwin(tk, 0.5, "Step", 1e-5, {0, 1, 0}) == -1);
            CHECK(FindTakeTwin(tk, 1.5, "Step", 1e-5, {0, 1}) == 2);  // a short skip list
            // A take-marker edit finds the project ref of its event.
            std::vector<ProjectMarkerRef> pr = refs;
            CHECK(FindProjectTwin(pr, t1.t, t1.name, 1e-5, {}) == 0);
            CHECK(FindProjectTwin(pr, t2.t, t2.name, 1e-5, {}) == 1);
            CHECK(FindProjectTwin(pr, t0.t, t0.name, 1e-5, {}) == -1);  // another name
            CHECK(FindProjectTwin(pr, 1.0, "Step", 1e-5, {}) == -1);    // another time
            CHECK(FindProjectTwin(pr, 0.5, "Step", 1e-5, {1, 0}) == -1);  // skipped
            // A hidden ref (no GUID) is still a twin; an older ref (no clip time) never is.
            pr[0].guid.clear();
            CHECK(FindProjectTwin(pr, 0.5, "Step", 1e-5, {}) == 0);
            pr[1].has_c = false;
            CHECK(FindProjectTwin(pr, 1.5, "Step", 1e-5, {}) == -1);
        }
    }

    if (g_fails) {
        std::printf("event_list_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("event_list_test: all passed\n");
    return 0;
}
