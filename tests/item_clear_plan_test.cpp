// SPDX-License-Identifier: MIT
//
// Host test of src/item_clear_plan.h (13-26): what "Clear item data" removes, keeps and counts,
// over the spec's matrix (linked copies, made unique, attached + library source, stray tag,
// unreadable record, nothing), and the confirm band's text.

#include "item_clear_plan.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "rule_record.h"
#include "sound_record.h"

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

static bool Has(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

// A populated, committed record: 2 rules, 2 detected + 1 user + 1 suppress events, 2 take
// markers, 2 project markers (1 hidden), 1 preview project marker.
static std::string RulesRecord(const char* pool_line = nullptr)
{
    std::string s = "RAVRULES 1\n";
    if (pool_line) s += std::string(pool_line) + "\n";
    s += "block color=#5F9EDD hold_ms=0 cooldown_ms=250 offset_ms=0 land=cross marker=Step\n"
         "cond q=point bones=role:left_heel comb=lowest ref=floor meas=position axis=vertical dir=below thr=0.05 margin=0.025 fixed=0\n"
         "block color=#5F9EDD hold_ms=0 cooldown_ms=250 offset_ms=0 land=cross marker=Jump\n"
         "cond q=point bones=role:left_heel comb=lowest ref=floor meas=position axis=vertical dir=below thr=0.05 margin=0.025 fixed=0\n"
         "event t=1.25 kind=detected block=0\n"
         "event t=2.5 kind=detected block=0\n"
         "event t=3 kind=user block=1\n"
         "event t=3.5 kind=suppress block=0\n"
         "applied markers=both sig=ab item={A}\n"
         "tmarker t=1.25 name=Step\n"
         "tmarker t=2.5 name=Step\n"
         "pmarker guid={M1} t=11.25\n"
         "pmarker guid= t=12.5\n"
         "preview markers=both sig=cd item={A}\n"
         "ppmarker guid={M2} t=13\n";
    return s;
}

static std::string SoundsRecord(const std::vector<std::string>& placed)
{
    ItemSounds s;
    s.seed = 42;
    EventSounds ev;
    ev.rid = "0123456789abcdef";
    ev.t = 1.25;
    for (const std::string& g : placed) {
        SoundPlacement p;
        p.item_guid = g;
        p.node = "n1";
        ev.placed.push_back(p);
    }
    s.events.push_back(ev);
    return SerializeItemSounds(s);
}

static std::string Tag(const char* owner, Ownership own = Ownership::Populated)
{
    PlacedTag t;
    t.owner = owner;
    t.rid = "0123456789abcdef";
    t.t = 1.25;
    t.node = "n1";
    t.own = own;
    return SerializePlacedTag(t);
}

static ClearTarget Target(const char* guid, const std::string& rules, const std::string& sounds = {},
                          const std::string& committed = {})
{
    ClearTarget t;
    t.guid = guid;
    t.rules_raw = rules;
    t.sounds_raw = sounds;
    t.committed_raw = committed;
    return t;
}

static ClearTagged Tagged(const char* guid, const std::string& tag)
{
    ClearTagged t;
    t.guid = guid;
    t.tag_raw = tag;
    return t;
}

// The plan with the sound library (the cases below are the sounds-on behaviour, also in the OFF twin).
static ItemClearPlan PlanOn(const std::vector<ClearTarget>& targets, const std::vector<ClearTagged>& tagged,
                            const std::vector<std::string>& sources)
{
    return PlanItemClear(targets, tagged, sources, /*with_sounds=*/true);
}

static void TestPoolOnlyRecord()
{
    CHECK(PoolOnlyRecord("").empty());
    CHECK(PoolOnlyRecord(RulesRecord()).empty());       // no pool line: the key goes
    CHECK(PoolOnlyRecord("not a record\n").empty());    // unreadable: the key goes
    const std::string keep = PoolOnlyRecord(RulesRecord("pool id={X}"));
    CHECK(keep.find("RAVRULES 1") == 0);
    CHECK(keep.find("pool id={X}") != std::string::npos);
    CHECK(keep.find("block") == std::string::npos);
    CHECK(keep.find("event") == std::string::npos);
    CHECK(keep.find("marker") == std::string::npos);
    // It reads back as a made-unique record with no rules, and is stable.
    ItemRules r;
    CHECK(ParseItemRules(keep, &r));
    CHECK(r.has_pool && r.pool_id == "{X}");
    CHECK(r.blocks.empty() && r.events.empty() && r.tmarkers.empty() && r.pmarkers.empty());
    CHECK(PoolOnlyRecord(keep) == keep);
}

// Linked copies: the item + 2 pool copies, each populated: all 3 records go, all their sounds.
static void TestLinkedCopies()
{
    const std::vector<ClearTarget> targets = {
        Target("{A}", RulesRecord(), SoundsRecord({"{S1}", "{S2}"}), RulesRecord()),
        Target("{B}", RulesRecord(), SoundsRecord({"{S3}"})),
        Target("{C}", RulesRecord(), SoundsRecord({"{S4}"})),
    };
    const std::vector<ClearTagged> tagged = {
        Tagged("{S1}", Tag("{A}")), Tagged("{S2}", Tag("{A}")), Tagged("{S3}", Tag("{B}")),
        Tagged("{S4}", Tag("{C}")), Tagged("{OTHER}", Tag("{Z}")),
    };
    const ItemClearPlan p = PlanOn(targets, tagged, {});
    CHECK(p.anything);
    CHECK(p.counts.items == 3 && p.counts.copies == 2);
    CHECK(p.counts.events == 9);   // 3 per record (the suppression is no event)
    CHECK(p.counts.rules == 6);
    CHECK(p.counts.markers == 12);  // 2 take + 1 shown project + 1 preview, per record
    CHECK(p.counts.sounds == 4 && p.counts.attached == 0);
    CHECK(p.delete_guids.size() == 4 && !Has(p.delete_guids, "{OTHER}"));
    CHECK(p.untag_guids.empty());
    CHECK(p.records.size() == 3);
    for (const ClearRecordWrite& w : p.records) {
        CHECK(w.rules && w.rules_text.empty());
        CHECK(w.sounds);
    }
    CHECK(p.records[0].committed && !p.records[1].committed);
    CHECK(p.markers.size() == 3 && p.markers[0] && p.markers[1] && p.markers[2]);
    CHECK(ClearConfirmTitle(p.counts.copies) == "Clear this item and its 2 linked copies?");
    CHECK(ClearConfirmText(p.counts) ==
          "9 events \xC2\xB7 6 rules \xC2\xB7 12 markers \xC2\xB7 4 sounds are removed. One undo step.");
    CHECK(ClearDoneText(p.counts) == "Cleared: 1 item + 2 copies \xC2\xB7 4 sounds removed");
}

// Made unique: only the item is a target; its record keeps `pool id=X`.
static void TestMadeUnique()
{
    const ItemClearPlan p =
        PlanOn({Target("{A}", RulesRecord("pool id={X}"), SoundsRecord({"{S1}"}))}, {Tagged("{S1}", Tag("{A}"))}, {});
    CHECK(p.records.size() == 1);
    CHECK(p.records[0].rules && p.records[0].rules_text == PoolOnlyRecord(RulesRecord("pool id={X}")));
    CHECK(!p.records[0].rules_text.empty());
    CHECK(p.counts.copies == 0);
    CHECK(ClearConfirmTitle(0) == "Clear this item?");
    // Cleared once: nothing left to clear.
    const ItemClearPlan again = PlanOn({Target("{A}", p.records[0].rules_text)}, {}, {});
    CHECK(!again.anything);
}

// Attached + library source: 2 attached items, one is a Project Library Sound's source.
static void TestAttachedAndLibrarySource()
{
    const ItemClearPlan p = PlanOn({Target("{A}", RulesRecord(), SoundsRecord({"{P1}", "{T1}", "{T2}"}))},
                                          {Tagged("{P1}", Tag("{A}")), Tagged("{T1}", Tag("{A}", Ownership::Attached)),
                                           Tagged("{T2}", Tag("{A}", Ownership::Attached))},
                                          {"{t2}"});  // case ignored
    CHECK(p.counts.sounds == 2 && p.counts.attached == 1);
    CHECK(Has(p.delete_guids, "{P1}") && Has(p.delete_guids, "{T1}") && !Has(p.delete_guids, "{T2}"));
    CHECK(p.untag_guids.size() == 1 && p.untag_guids[0] == "{T2}");
    CHECK(ClearConfirmText(p.counts).find("2 sounds (1 attached by you) are removed") != std::string::npos);
}

// Stray tag: owned by the item but not in its record -> deleted. A GUID the record lists whose
// tag names another owner -> deleted too. An untagged item is never in the list (the user's).
static void TestStrayTag()
{
    const ItemClearPlan p = PlanOn({Target("{a}", RulesRecord(), SoundsRecord({"{L1}"}))},
                                          {Tagged("{STRAY}", Tag("{A}")), Tagged("{L1}", Tag("{Q}")),
                                           Tagged("{FOREIGN}", Tag("{Q}"))},
                                          {});
    CHECK(Has(p.delete_guids, "{STRAY}"));
    CHECK(Has(p.delete_guids, "{L1}"));
    CHECK(!Has(p.delete_guids, "{FOREIGN}"));
    CHECK(p.counts.sounds == 2);
}

// Unreadable record: its keys cleared, sounds by tag deleted, no marker readable (none deleted).
static void TestUnreadable()
{
    const ItemClearPlan p = PlanOn({Target("{A}", "RAVRULES-NEXT whatever\nblock\n", "garbage", "garbage")},
                                          {Tagged("{S1}", Tag("{A}"))}, {});
    CHECK(p.anything);
    CHECK(p.records.size() == 1);
    CHECK(p.records[0].rules && p.records[0].rules_text.empty());
    CHECK(p.records[0].committed && p.records[0].sounds);
    CHECK(p.markers.size() == 1 && !p.markers[0]);
    CHECK(p.counts.markers == 0 && p.counts.events == 0 && p.counts.rules == 0);
    CHECK(p.counts.sounds == 1);
}

// Nothing: a fresh item.
static void TestNothing()
{
    const ItemClearPlan p = PlanOn({Target("{A}", "")}, {Tagged("{S}", Tag("{Z}"))}, {});
    CHECK(!p.anything);
    CHECK(p.records.empty() && p.delete_guids.empty() && p.untag_guids.empty());
}

static void TestLibrarySourceGuids()
{
    SoundStore store;
    SoundLibrary a, b;
    SoundNode item_sound;
    item_sound.type = SoundNodeType::Sound;
    item_sound.sound.source.kind = SoundSourceKind::Item;
    item_sound.sound.source.item_guid = "{I1}";
    SoundNode file_sound;  // converted to a User Library (13-24): a File source that keeps its item
    file_sound.type = SoundNodeType::Sound;
    file_sound.sound.source.kind = SoundSourceKind::File;
    file_sound.sound.source.item_guid = "{F1}";
    SoundNode file_only;  // a File Sound with no project item
    file_only.type = SoundNodeType::Sound;
    file_only.sound.source.kind = SoundSourceKind::File;
    SoundNode random;  // not a Sound: never a source
    random.type = SoundNodeType::Random;
    random.sound.source.item_guid = "{R1}";
    a.nodes = {item_sound, random};
    b.nodes = {file_sound, file_only};
    store.libraries = {a, b};
    const std::vector<std::string> g = LibrarySourceGuids(store);
    CHECK(g.size() == 2 && Has(g, "{I1}") && Has(g, "{F1}") && !Has(g, "{R1}"));
    // Both stay on the timeline (untagged), not deleted.
    const ItemClearPlan p = PlanOn({Target("{A}", RulesRecord())},
                                          {Tagged("{I1}", Tag("{A}", Ownership::Attached)), Tagged("{F1}", Tag("{A}"))}, g);
    CHECK(p.delete_guids.empty() && p.untag_guids.size() == 2);
    CHECK(ClearDoneText(p.counts) == "Cleared: 1 item");
}

static void TestTexts()
{
    ItemClearCounts c;
    c.items = 1;
    CHECK(ClearConfirmText(c) == "Its settings are removed. One undo step.");
    c.rules = 1;
    CHECK(ClearConfirmText(c) == "1 rule is removed. One undo step.");
    c.events = 42;
    c.rules = 12;
    c.markers = 84;
    c.sounds = 37;
    c.attached = 3;
    CHECK(ClearConfirmText(c) == "42 events \xC2\xB7 12 rules \xC2\xB7 84 markers \xC2\xB7 37 sounds (3 attached by you) "
                                 "are removed. One undo step.");
    CHECK(ClearConfirmTitle(1) == "Clear this item and its 1 linked copy?");
    ItemClearCounts d;
    d.copies = 1;
    d.sounds = 1;
    CHECK(ClearDoneText(d) == "Cleared: 1 item + 1 copy \xC2\xB7 1 sound removed");
}

// Release 0.4.0: RAV built without the sound library (with_sounds false) clears the rules, events and
// markers only: the sound records stay, no sound item is deleted or untagged, the texts name no sound.
// With the sounds (the default), the same input clears them as before.
static void TestWithoutSounds()
{
    const std::vector<ClearTarget> targets = {
        Target("{A}", RulesRecord(), SoundsRecord({"{S1}", "{P1}"}), RulesRecord()),
        Target("{B}", RulesRecord(), SoundsRecord({"{S3}"})),
    };
    const std::vector<ClearTagged> tagged = {Tagged("{S1}", Tag("{A}", Ownership::Attached)), Tagged("{P1}", Tag("{A}")),
                                             Tagged("{S3}", Tag("{B}")), Tagged("{STRAY}", Tag("{A}"))};
    const ItemClearPlan off = PlanItemClear(targets, tagged, {"{P1}"}, /*with_sounds=*/false);
    CHECK(off.anything);
    CHECK(off.delete_guids.empty() && off.untag_guids.empty());
    CHECK(off.counts.sounds == 0 && off.counts.attached == 0);
    CHECK(off.counts.events == 6 && off.counts.rules == 4 && off.counts.markers == 8);
    CHECK(off.records.size() == 2);
    for (const ClearRecordWrite& w : off.records) CHECK(w.rules && !w.sounds);
    CHECK(off.records[0].committed);
    CHECK(ClearConfirmText(off.counts).find("sound") == std::string::npos);
    CHECK(ClearDoneText(off.counts).find("sound") == std::string::npos);
    // Only sound data: nothing to clear without the sounds.
    const ItemClearPlan only = PlanItemClear({Target("{A}", {}, SoundsRecord({"{S1}"}))}, {Tagged("{S1}", Tag("{A}"))}, {}, false);
    CHECK(!only.anything && only.records.empty());

    const ItemClearPlan on = PlanItemClear(targets, tagged, {"{P1}"}, /*with_sounds=*/true);
    CHECK(on.counts.sounds == 3 && on.counts.attached == 1);
    CHECK(Has(on.delete_guids, "{S1}") && Has(on.delete_guids, "{S3}") && Has(on.delete_guids, "{STRAY}"));
    CHECK(on.untag_guids.size() == 1 && on.untag_guids[0] == "{P1}");
    for (const ClearRecordWrite& w : on.records) CHECK(w.sounds);
    const ItemClearPlan dflt = PlanItemClear(targets, tagged, {"{P1}"});  // the default is this build's (ON in the host tests)
    CHECK(dflt.counts.sounds == (kWithSounds ? 3 : 0));
}

int main()
{
    TestPoolOnlyRecord();
    TestLinkedCopies();
    TestMadeUnique();
    TestAttachedAndLibrarySource();
    TestStrayTag();
    TestUnreadable();
    TestNothing();
    TestLibrarySourceGuids();
    TestTexts();
    TestWithoutSounds();
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("item_clear_plan: all passed\n");
    return 0;
}
