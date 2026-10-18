// SPDX-License-Identifier: MIT
//
// Host test of src/sound_sync_plan.h (13-35): node prints and what makes a placed sound behind (volume, Blend offset,
// fades, zones, an excluded variation; measured data never), the change texts, the hand edits and what Keep /
// Overwrite do with them, the project's rule token, and the Library's view: the card, the marks, the where-dialog's
// lines, the hand list and the toast (the mockup's Stone / Grass / Jump_Land scenario).

#include "sound_sync_plan.h"

#include <cmath>
#include <cstdio>
#include <string>

#include "sound_record.h"

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)
static bool Near(double a, double b, double e = 1e-9) { return std::fabs(a - b) <= e; }

static SoundNode Node(const char* uid, const char* parent, SoundNodeType type, const char* name, double vol = 0.0)
{
    SoundNode n;
    n.uid = uid;
    n.parent = parent;
    n.type = type;
    n.name = name;
    n.volume_db = vol;
    return n;
}

static SoundNode Sound(const char* uid, const char* parent, const char* name, int frames, double vol = 0.0)
{
    SoundNode n = Node(uid, parent, SoundNodeType::Sound, name, vol);
    for (int k = 0; k < frames; ++k) {
        SoundFrame f;
        f.start = k * 1.0;
        f.length = 0.5;
        EditZone z;
        z.a = f.start;
        z.b = f.start + f.length;
        z.part = kWholePart;
        f.zones.push_back(z);
        n.sound.frames.push_back(f);
    }
    return n;
}

// Footsteps: root Random > LocomotionType Switch (Run, Walk) > Surface Switch > Stone / Grass Sounds; Jump_Land Blend >
// Body / Cloth Sounds.
static SoundStore Store()
{
    SoundStore  st;
    SwitchGroup loco;
    loco.uid = "gLoco";
    loco.name = "LocomotionType";
    loco.values = {SwitchValue{"vRun", "Run", {}, {}}, SwitchValue{"vWalk", "Walk", {}, {}}};
    SwitchGroup surf;
    surf.uid = "gSurf";
    surf.name = "Surface Material";
    surf.values = {SwitchValue{"vStone", "Stone", {}, {}}, SwitchValue{"vGrass", "Grass", {}, {}}};
    st.groups = {loco, surf};
    SoundLibrary lib;
    lib.uid = "libFS";
    lib.name = "Footsteps";
    lib.root = "root";
    lib.nodes.push_back(Node("root", "", SoundNodeType::Random, "Footsteps"));
    SoundNode sw = Node("loco", "root", SoundNodeType::Switch, "LocomotionType");
    sw.group_uid = "gLoco";
    sw.cases = {SwitchCase{"vRun", "runS", {}}, SwitchCase{"vWalk", "walkS", {}}};
    lib.nodes.push_back(sw);
    SoundNode rs = Node("runS", "loco", SoundNodeType::Switch, "Surface Material");
    rs.group_uid = "gSurf";
    rs.cases = {SwitchCase{"vStone", "runStone", {}}};
    lib.nodes.push_back(rs);
    SoundNode ws = Node("walkS", "loco", SoundNodeType::Switch, "Surface Material");
    ws.group_uid = "gSurf";
    ws.cases = {SwitchCase{"vGrass", "walkGrass", {}}};
    lib.nodes.push_back(ws);
    lib.nodes.push_back(Sound("runStone", "runS", "Stone", 3, -3.0));
    lib.nodes.push_back(Sound("walkGrass", "walkS", "Grass", 6, -6.0));
    lib.nodes.push_back(Node("jump", "root", SoundNodeType::Blend, "Jump_Land", -2.0));
    SoundNode cloth = Sound("jCloth", "jump", "FS_Land_Cloth", 2);
    cloth.offset_ms = 12.0;
    lib.nodes.push_back(cloth);
    st.libraries.push_back(lib);
    return st;
}

static SoundNode* N(SoundStore& st, const char* uid) { return FindNode(st.libraries[0], uid); }

static PlacedWrite Wr(double vol = 0.5, double len = 0.5)
{
    PlacedWrite w;
    w.vol = vol;
    w.fade_in = 0.001;
    w.fade_out = 0.005;
    w.length = len;
    return w;
}

// A placed item of `owner` for node `node` variation `va`, stamped from `st`.
static SyncPlacedItem Placed(const SoundStore& st, const char* guid, const char* owner, const char* node, int va, double t)
{
    SyncPlacedItem it;
    it.guid = guid;
    it.owner = owner;
    it.name = std::string("snd_") + guid;
    it.tag.owner = owner;
    it.tag.rid = "r1";
    it.tag.t = t;
    it.tag.node = node;
    it.tag.own = Ownership::Populated;
    it.tag.va = va;
    it.tag.lib = PathPrints(*LibraryOfNode(st, node), node, va);
    it.tag.has_wr = true;
    it.tag.wr = Wr();
    it.now = Wr();
    it.anchor = t;
    return it;
}

int main()
{
    // ---- Prints and behind ---------------------------------------------------------------------
    {
        SoundStore     st = Store();
        const PlacedTag tag = Placed(st, "a", "i1", "runStone", 1, 1.0).tag;
        CHECK(tag.lib.size() == 4 && tag.lib[0].node == "runStone" && tag.lib[3].node == "root");
        CHECK(BehindNodes(tag, st).empty());
        // A volume change: behind on that node, described with both values.
        N(st, "runStone")->volume_db = -6.0;
        std::vector<std::string> b = BehindNodes(tag, st);
        CHECK(b.size() == 1 && b[0] == "runStone");
        const NodePrint now = PrintOf(*N(st, "runStone"), 1);
        CHECK(DescribeChange(tag.lib[0], now, 1) == "Volume \xE2\x88\x92" "3 dB \xE2\x86\x92 \xE2\x88\x92" "6 dB");
        CHECK(!ChangeRedraws(tag.lib[0], now));
        // A node above changes too: both behind, node first.
        N(st, "loco")->volume_db = 1.5;
        b = BehindNodes(tag, st);
        CHECK(b.size() == 2 && b[0] == "runStone" && b[1] == "loco");
        // Measured data (sync points, trims, an automatic snap) never makes it behind.
        SoundStore m = Store();
        N(m, "runStone")->sound.frames[1].syncs = {1.01};
        N(m, "runStone")->sound.frames[1].part_trims = {1.0, 1.4};
        N(m, "runStone")->sound.frames[1].zones[0].auto_snap.set = true;
        N(m, "runStone")->sound.frames[1].zones[0].auto_snap.t = 1.02;
        CHECK(BehindNodes(tag, m).empty());
        // Another variation's settings leave it alone; its own variation's fades, zones and level do not.
        N(m, "runStone")->sound.frames[0].db = -4.0;
        CHECK(BehindNodes(tag, m).empty());
        SoundStore f = Store();
        N(f, "runStone")->sound.frames[1].zones[0].fade_out = 0.04;
        CHECK(BehindNodes(tag, f).size() == 1 && DescribeChange(tag.lib[0], PrintOf(*N(f, "runStone"), 1), 1) == "fades");
        SoundStore z = Store();
        N(z, "runStone")->sound.frames[1].zones[0].b = 1.45;
        CHECK(DescribeChange(tag.lib[0], PrintOf(*N(z, "runStone"), 1), 1) == "zones");
        SoundStore l = Store();
        N(l, "runStone")->sound.frames[1].zones[0].db = -2.0;
        N(l, "runStone")->sound.fade_in = 0.003;
        CHECK(DescribeChange(tag.lib[0], PrintOf(*N(l, "runStone"), 1), 1) == "fades, level");
        // The variation it played excluded: a redraw, said with its number (1-based).
        SoundStore x = Store();
        N(x, "runStone")->sound.frames[1].on = false;
        const NodePrint off = PrintOf(*N(x, "runStone"), 1);
        CHECK(ChangeRedraws(tag.lib[0], off) && DescribeChange(tag.lib[0], off, 1) == "variation 2 excluded");
        // A structure change (the node moved, deleted) or an old tag: not behind (Populate's).
        SoundStore s = Store();
        N(s, "runStone")->parent = "walkS";
        N(s, "runStone")->volume_db = -9.0;
        CHECK(BehindNodes(tag, s).empty());
        SoundStore d = Store();
        d.libraries[0].nodes.erase(d.libraries[0].nodes.begin() + 4);
        CHECK(BehindNodes(tag, d).empty());
        PlacedTag old = tag;
        old.lib.clear();
        CHECK(BehindNodes(old, st).empty());
        // Blend offset.
        SoundStore o = Store();
        const PlacedTag ct = Placed(o, "c", "i4", "jCloth", 0, 2.0).tag;
        N(o, "jCloth")->offset_ms = 18.0;
        CHECK(DescribeChange(ct.lib[0], PrintOf(*N(o, "jCloth"), 0), 0) == "Blend offset 12 \xE2\x86\x92 18 ms");
        // A re-Populate applied the volume: every print's volume is adopted, whatever else differs (that stays behind).
        std::vector<NodePrint> stored = tag.lib;
        SoundStore             v = Store();
        N(v, "runStone")->volume_db = -6.0;
        CHECK(AdoptVolumes(stored, PathPrints(v.libraries[0], "runStone", 1)) && stored[0].vol == -6.0);
        PlacedTag adopted = tag;
        adopted.lib = stored;
        CHECK(BehindNodes(adopted, v).empty());
        std::vector<NodePrint> s2 = tag.lib;
        N(v, "runStone")->sound.frames[1].zones[0].db = 1.0;
        CHECK(AdoptVolumes(s2, PathPrints(v.libraries[0], "runStone", 1)) && s2[0].vol == -6.0);
        PlacedTag still = tag;
        still.lib = s2;
        CHECK(BehindNodes(still, v).size() == 1 &&
              DescribeChange(s2[0], PrintOf(*N(v, "runStone"), 1), 1) == "level");  // no "Volume" left in the chip
        // The kept item's tag after that Populate: prints adopted, what RAV wrote follows the ratio: nothing behind, no
        // hand edit.
        {
            SoundStore w = Store();
            PlacedTag  kt = tag;
            kt.has_wr = true;
            kt.wr = Wr(0.5);
            N(w, "runStone")->volume_db = -6.0;
            const double was = 0.5, now_vol = 0.5 * std::pow(10.0, -3.0 / 20.0);
            KeptTagAfterVolume(kt, PathPrints(w.libraries[0], "runStone", 1), was, now_vol);
            CHECK(BehindNodes(kt, w).empty());
            PlacedWrite item = Wr(now_vol);
            CHECK(Near(kt.wr.vol, now_vol) && !HandEdits(kt.wr, item, 0.0).Any());
            // A user's -4 dB on the item stays a hand edit.
            PlacedTag ut = tag;
            ut.has_wr = true;
            ut.wr = Wr(0.5);
            const double user = 0.5 * std::pow(10.0, -4.0 / 20.0);
            KeptTagAfterVolume(ut, PathPrints(w.libraries[0], "runStone", 1), user, user * std::pow(10.0, -3.0 / 20.0));
            PlacedWrite uitem = Wr(user * std::pow(10.0, -3.0 / 20.0));
            const HandEdit uh = HandEdits(ut.wr, uitem, 0.0);
            CHECK(uh.vol && Near(uh.vol_db, -4.0, 1e-6));
        }
        // Paths as the dialogs say them.
        CHECK(SyncNodePath(st, st.libraries[0], "runStone") == "Run \xE2\x80\xBA Stone");
        CHECK(SyncNodePath(st, st.libraries[0], "jCloth") == "Jump_Land \xE2\x80\xBA FS_Land_Cloth");
        CHECK(SyncNodePath(st, st.libraries[0], "root") == "Footsteps");
    }
    // ---- What a Sync does in Populate's engine --------------------------------------------------
    {
        CHECK(SyncStepAction(SlotAction::Keep, true) == SyncStep::KeepInPlace);
        CHECK(SyncStepAction(SlotAction::SetVariation, true) == SyncStep::Replace);
        CHECK(SyncStepAction(SlotAction::Place, true) == SyncStep::Skip);
        CHECK(SyncStepAction(SlotAction::Remove, true) == SyncStep::Skip);
        CHECK(SyncStepAction(SlotAction::ReplaceNode, true) == SyncStep::Skip);
        CHECK(SyncStepAction(SlotAction::Keep, false) == SyncStep::Skip);
        CHECK(SyncStepAction(SlotAction::SetVariation, false) == SyncStep::Skip);
        CHECK(SyncEventPlan(EventPlan::Draw, true, true) == EventPlan::Draw);
        CHECK(SyncEventPlan(EventPlan::Draw, true, false) == EventPlan::Keep);
        CHECK(SyncEventPlan(EventPlan::Draw, false, false) == EventPlan::Skip);
        CHECK(SyncEventPlan(EventPlan::Keep, true, true) == EventPlan::Keep);
        CHECK(SyncEventPlan(EventPlan::Skip, true, true) == EventPlan::Skip);
    }
    // ---- Hand edits ------------------------------------------------------------------------------
    {
        const PlacedWrite wr = Wr(0.5, 0.5);
        CHECK(!HandEdits(wr, wr, 0.0).Any());
        PlacedWrite now = wr;
        now.vol = 0.5 * std::pow(10.0, -4.0 / 20.0);
        HandEdit h = HandEdits(wr, now, 0.0);
        CHECK(h.vol && !h.fades && !h.length && !h.moved && Near(h.vol_db, -4.0, 1e-6) && h.text == "volume \xE2\x88\x92" "4 dB");
        now = wr;
        now.fade_out = 0.04;
        h = HandEdits(wr, now, 0.012);
        CHECK(h.fades && h.moved && h.text == "moved +12 ms, fade-out 40 ms");
        now = wr;
        now.vol = 0.5 * std::pow(10.0, 2.0 / 20.0);
        now.length = 0.4;
        CHECK(HandEdits(wr, now, 0.0).text == "volume +2 dB, length");
        now = wr;
        now.out_dir = 0.5;
        CHECK(HandEdits(wr, now, 0.0).text == "fade shape");
        // Within the tolerances: no edit.
        now = wr;
        now.vol = 0.5 * std::pow(10.0, 0.01 / 20.0);
        now.length = 0.5002;
        CHECK(!HandEdits(wr, now, 0.0002).Any());
        // Keep: the user's offset kept on top of the library's change; their fades / length / nudge stay. Overwrite:
        // the library's values, nudge 0.
        now = wr;
        now.vol = 0.25;  // -6 dB by hand
        now.fade_out = 0.04;
        h = HandEdits(wr, now, 0.012);
        SyncItemPlan k = PlanSyncItem(wr, now, h, false, -6.0, -3.0, 0.012);
        const double three = std::pow(10.0, -3.0 / 20.0);
        CHECK(Near(k.vol, 0.25 * three) && Near(k.wr_vol, 0.5 * three) && !k.set_fades && k.set_length && Near(k.nudge_s, 0.012));
        SyncItemPlan ow = PlanSyncItem(wr, now, h, true, -6.0, -3.0, 0.012);
        CHECK(Near(ow.vol, 0.5 * three) && ow.set_fades && ow.set_length && ow.nudge_s == 0.0);
        SyncItemPlan plain = PlanSyncItem(wr, wr, HandEdit{}, false, -3.0, -3.0, 0.0);
        CHECK(Near(plain.vol, 0.5) && plain.set_fades && plain.set_length);
    }
    // ---- The rule token --------------------------------------------------------------------------
    {
        SoundStore st;
        st.kept = {"loopvar=same"};
        CHECK(HandEditRuleOf(st) == HandEditRule::Ask && !SetHandEditRule(st, HandEditRule::Ask));
        CHECK(SetHandEditRule(st, HandEditRule::Keep) && HandEditRuleOf(st) == HandEditRule::Keep && st.kept.size() == 2);
        CHECK(SetHandEditRule(st, HandEditRule::Overwrite) && HandEditRuleOf(st) == HandEditRule::Overwrite && st.kept.size() == 2);
        CHECK(SetHandEditRule(st, HandEditRule::Ask) && st.kept.size() == 1 && st.kept[0] == "loopvar=same");
        // It travels in the store's text.
        SetHandEditRule(st, HandEditRule::Keep);
        SoundStore with = Store();
        with.kept = st.kept;
        SoundStore back;
        CHECK(ParseSoundStore(SerializeSoundStore(with), &back) && HandEditRuleOf(back) == HandEditRule::Keep);
        CHECK(std::string(HandEditRuleText(HandEditRule::Overwrite)) == "Overwrite them");
    }
    // ---- The Library's view: the mockup's scenario ---------------------------------------------
    {
        SoundStore                  st = Store();
        std::vector<SyncPlacedItem> items;
        // Knight_Run_01 (i1): 2 Stone sounds (one of 2 pieces), one edited by hand; Knight_Run_02 (i2): 1 Stone;
        // Knight_Walk (i3, Guard track): 2 Grass sounds, variations 2 and 4; i4 a Cloth.
        items.push_back(Placed(st, "a1", "i1", "runStone", 0, 1.0));
        items.push_back(Placed(st, "a2", "i1", "runStone", 0, 1.0));  // the second piece of a1's sound
        items.push_back(Placed(st, "a3", "i1", "runStone", 2, 2.0));
        items.back().now.vol = 0.5 * std::pow(10.0, -4.0 / 20.0);
        items.push_back(Placed(st, "b1", "i2", "runStone", 1, 1.0));
        items.push_back(Placed(st, "g1", "i3", "walkGrass", 2, 1.0));
        items.push_back(Placed(st, "g2", "i3", "walkGrass", 4, 3.0));
        items.push_back(Placed(st, "c1", "i4", "jCloth", 0, 1.0));
        SyncPlacedItem old = Placed(st, "o1", "i2", "runStone", 0, 5.0);
        old.tag.lib.clear();  // an older placement: never counted
        items.push_back(old);
        const std::vector<SyncOwner> owners = {{"i1", "Knight_Run_01", "Knight", true},
                                               {"i3", "Knight_Walk_Loop", "Guard", false},
                                               {"i2", "Knight_Run_02", "Knight", true},
                                               {"i4", "Knight_Jump", "Knight", false}};
        // Nothing changed: the card is clean.
        SyncState s0 = BuildSyncState(st, items, owners);
        CHECK(s0.sounds.empty() && SyncCardOf(s0, "libFS").changes == 0);
        // Stone -3 -> -6 dB, Grass variation 3 off (0-based 2), Cloth's offset 12 -> 18 ms.
        N(st, "runStone")->volume_db = -6.0;
        N(st, "walkGrass")->sound.frames[2].on = false;
        N(st, "jCloth")->offset_ms = 18.0;
        const SyncState s = BuildSyncState(st, items, owners);
        CHECK(s.sounds.size() == 5);  // a1 + a2 one sound, a3, b1, g1 (variation 3), c1; g2 plays variation 5: not behind
        const SyncCard c = SyncCardOf(s, "libFS");
        CHECK(c.changes == 3 && c.sounds == 5 && c.items == 4);
        CHECK(SyncCardText(c) == "3 changes not yet on 5 placed sounds in 4 items.");
        CHECK(SyncCardText(SyncCard{1, 1, 1}) == "1 change not yet on 1 placed sound in 1 item.");
        CHECK(SyncCardOf(s, "other").sounds == 0);
        const std::map<std::string, int> marks = SyncMarksOf(s, "libFS");
        CHECK(marks.size() == 3 && marks.at("runStone") == 3 && marks.at("walkGrass") == 1 && marks.at("jCloth") == 1);
        CHECK(s.what.at("walkGrass") == "variation 3 excluded" && s.redraws.count("walkGrass") && !s.redraws.count("runStone"));
        CHECK(s.what.at("runStone") == "Volume \xE2\x88\x92" "3 dB \xE2\x86\x92 \xE2\x88\x92" "6 dB");
        // The where-dialog: grouped by track (owners' order: Knight first), the node scope narrows it.
        std::vector<SyncItemLine> lines = SyncItemLines(s, "libFS", "");
        CHECK(lines.size() == 4 && lines[0].owner == "i1" && lines[1].owner == "i2" && lines[2].owner == "i4" &&
              lines[3].owner == "i3");
        CHECK(lines[0].sounds == 2 && lines[0].hand == 1 && lines[0].selected && lines[0].nodes.size() == 1);
        lines = SyncItemLines(s, "libFS", "runStone");
        CHECK(lines.size() == 2 && lines[0].owner == "i1" && lines[1].owner == "i2");
        // The hand list of the chosen items in scope.
        CHECK(SyncHandSounds(s, "libFS", "", {"i1", "i2"}).size() == 1);
        CHECK(SyncHandSounds(s, "libFS", "", {"i2"}).empty());
        CHECK(SyncHandSounds(s, "libFS", "walkGrass", {"i1"}).empty());
        const SyncSound* hs = SyncHandSounds(s, "libFS", "", {"i1"})[0];
        CHECK(hs->hand.text == "volume \xE2\x88\x92" "4 dB" && hs->items.size() == 1 && hs->items[0] == "a3");
        // The buttons.
        CHECK(!SyncApplyCheckedEnabled(0, 4) && SyncApplyCheckedEnabled(2, 4) && !SyncApplyCheckedEnabled(4, 4));
        CHECK(SyncRememberEnabled({}) && SyncRememberEnabled({0, 0}) && SyncRememberEnabled({1, 1}) &&
              !SyncRememberEnabled({0, 1}));
        // The toast.
        SyncCounts n;
        n.adjusted = 26;
        n.redrawn = 3;
        n.kept = 1;
        n.overwritten = 2;
        CHECK(SyncToastText(n, false) ==
              "One undo point \xE2\x80\x9CRAV: Sync placed sounds\xE2\x80\x9D: 26 adjusted, 3 redrawn, 1 hand edit kept, 2 overwritten");
        CHECK(SyncToastText(SyncCounts{}, true).find("0 adjusted \xC2\xB7 remembered") != std::string::npos);
        CHECK(SyncDbText(-4.0) == "\xE2\x88\x92" "4 dB" && SyncDbText(2.5) == "+2.5 dB" && SyncDbText(0.0) == "0 dB");
        // A Populated tag only (an Attached sound is the user's).
        std::vector<SyncPlacedItem> att = {items[4]};
        att[0].tag.own = Ownership::Attached;
        CHECK(BuildSyncState(st, att, owners).sounds.empty());
    }
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("sound_sync_plan: all checks passed\n");
    return 0;
}
