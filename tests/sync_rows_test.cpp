// SPDX-License-Identifier: MIT
//
// Host tests of src/sync_rows.h (story 13-77c readability pass): the window's row grouping (added / removed branch,
// edited Sound) and the display names / values.

#include "sync_rows.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "library_sync_select.h"

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
            ++g_fail;                                                  \
        }                                                              \
    } while (0)

static SyncChange& Add(SyncDiff* d, const std::string& key, SyncSide side, const std::string& node, const std::string& label,
                       const std::string& here, const std::string& file, std::vector<std::string> needs = {}, bool adds = false,
                       const std::string& subject = "", const std::string& text = "")
{
    SyncChange c;
    c.key = key;
    c.side = side;
    c.node = node;
    c.subject = subject.empty() ? node : subject;
    c.label = label;
    c.here_text = here;
    c.file_text = file;
    c.needs = std::move(needs);
    c.adds_container = adds;
    c.text = text;
    d->changes.push_back(c);
    return d->changes.back();
}

static const SyncRowGroup* GroupOf(const std::vector<SyncRowGroup>& gs, const std::string& key)
{
    for (const SyncRowGroup& g : gs)
        for (const std::string& k : g.keys)
            if (k == key) return &g;
    return nullptr;
}

// A Switch value Gravel added in this project: the Random it plays (2 Sounds), the case, the value only that case uses;
// plus Mud added in the file (a value two cases use is NOT absorbed); plus a Sound with variation / part changes.
static SyncDiff Fixture()
{
    SyncDiff d;
    d.root = "R";
    d.mine_nodes = {"R", "sw", "snd"};
    d.parent = {{"sw", "R"}, {"snd", "R"}, {"rg", "sw"}, {"s1", "rg"}, {"s2", "rg"}};
    Add(&d, "X|rg", SyncSide::Here, "rg", "", "present", "absent", {}, true, "rg", "Random  added");
    d.changes.back().label = "rg";  // an unnamed node: the engine's label is its uid
    d.changes.back().depth = 2;
    Add(&d, "X|s1", SyncSide::Here, "rg", "FS_Gravel_01", "present", "absent", {"X|rg"}, false, "s1", "Sound FS_Gravel_01 added").depth = 3;
    Add(&d, "X|s2", SyncSide::Here, "rg", "FS_Gravel_02", "present", "absent", {"X|rg"}, false, "s2", "Sound FS_Gravel_02 added").depth = 3;
    Add(&d, "C|sw|Gravel", SyncSide::Here, "sw", "Switch value Gravel", "plays rg", "absent", {"X|rg", "VX|g|Gravel"}, false, "sw");
    Add(&d, "GX|g", SyncSide::Here, "grp:g", "Surface", "present", "absent");
    Add(&d, "VX|g|Gravel", SyncSide::Here, "grp:g", "Gravel", "present", "absent", {"GX|g"});
    Add(&d, "VX|g|Mud", SyncSide::Here, "grp:g", "Mud", "present", "absent", {"GX|g"});
    Add(&d, "C|sw|Mud", SyncSide::Here, "sw", "Switch value Mud", "plays rg", "absent", {"X|rg", "VX|g|Mud"}, false, "sw");
    Add(&d, "C|sw2|Mud", SyncSide::Here, "sw2", "Switch value Mud", "plays x", "absent", {"VX|g|Mud"}, false, "sw2");
    Add(&d, "F|snd|gain", SyncSide::Here, "snd", "Gain", "-3 dB", "0 dB");
    Add(&d, "V|snd|a", SyncSide::Here, "R", "Variation 1", "present", "present", {"X|snd"}, false, "snd");
    Add(&d, "V|snd|b", SyncSide::File, "R", "Variation 2", "present", "present", {"X|snd"}, false, "snd");
    Add(&d, "V|snd|c", SyncSide::Here, "R", "Variation 3", "present", "absent", {"X|snd"}, false, "snd");
    Add(&d, "P|snd", SyncSide::File, "R", "Parts", "here", "file", {}, false, "snd");
    Add(&d, "X|old", SyncSide::File, "sw", "OldRandom", "present", "removed", {"C|sw|Old"}, true, "old", "Random OldRandom removed (3 sounds)");
    Add(&d, "C|sw|Old", SyncSide::File, "sw", "Switch value Old", "absent", "plays old", {"X|old"}, false, "sw");
    d.BuildIndex();
    return d;
}

// ---- 13-77: what changed in a Sound's variations, in words -------------------------------------------------------

static const char* kBaseTxt =
    "RAVLIB 2 by=A at=10\n"
    "group uid=G1 name=Surface\n"
    "value uid=V1 name=Concrete\n"
    "library uid=L1 name=Foot root=R\n"
    "node uid=R type=random name=Footstep\n"
    "node uid=S1 type=sound parent=R name=FS_C_01\n"
    "sound kind=file\n"
    "frame start=0.5 len=0.3\n"
    "frame start=1.5 len=0.3\n"
    "frame start=2.5 len=0.3\n"
    "frame start=3.5 len=0.3\n";

static std::string Rep(std::string t, const std::string& from, const std::string& to)
{
    const size_t p = t.find(from);
    if (p == std::string::npos) {
        std::printf("FAIL fixture: '%s' not in text\n", from.c_str());
        ++g_fail;
        return t;
    }
    t.replace(p, from.size(), to);
    return t;
}

static void TestEditedSoundWords()
{
    const std::string base = kBaseTxt;
    // here: three variations moved (length), one with a gain; the hand-edited flag comes with them
    std::string mine = Rep(base, "frame start=0.5 len=0.3", "frame start=0.5 len=0.38");
    mine = Rep(mine, "frame start=1.5 len=0.3", "frame start=1.5 len=0.25 db=-3");
    mine = Rep(mine, "frame start=2.5 len=0.3", "frame start=2.5 len=0.31");
    mine = Rep(mine, "sound kind=file\n", "sound kind=file edited=1\n");
    const SyncDiff d = DiffLibrary(base, mine, base);
    const auto     gs = SyncGroupChanges(d);
    CHECK(gs.size() == 1 && gs[0].kind == SyncRowKind::Edited);
    const SyncRowGroup& g = gs[0];
    CHECK(g.keys.size() == 4);  // 3 variations + the flag: no row of its own
    CHECK(g.moved == 3 && g.gains == 1 && !g.hand_only);
    CHECK(SyncEditSummary(g) == "3 variations moved, 1 gain");
    CHECK(g.here_edits == 4 && g.file_edits == 0);
    CHECK(SyncSideSummary(g, false) == "4 edits" && SyncSideSummary(g, true) == "unchanged");
    // the strip: per variation its field lines, each side's value in its column
    const auto lines = SyncEditDetailLines(d, g);
    bool       len_line = false, gain_line = false, hand_line = false;
    for (const SyncDetailLine& l : lines) {
        if (l.name == "Length" || l.name == "Variation 1 \xC2\xB7 Length") len_line = len_line || (l.project == "0.300 \xE2\x86\x92 0.380 s" && l.global == "0.300 s");
        if (l.name == "Gain") gain_line = l.project == "0.0 \xE2\x86\x92 \xE2\x88\x92" "3.0 dB" && l.global == "0.0 dB";
        if (l.name.find("by hand") != std::string::npos) hand_line = true;
    }
    CHECK(len_line && gain_line && !hand_line);
    // one tick takes the flag with the variations (the group's keys)
    SyncSelection sel;
    sel.KeepAll(d, g.keys, SyncDir::Up);
    CHECK(sel.Picks(d).count("F|S1|sound|edited") == 1 && sel.Picks(d).size() == 4);
    // the footer names the flag only when nothing of its Sound goes with it
    std::string full;
    const std::string foot = SyncFooterSummary(sel.Footer(d), &full);
    CHECK(full.find("by hand") == std::string::npos && full.find("edited changed") == std::string::npos);

    // the flag alone: the row reads "zones edited by hand", never a setting
    const SyncDiff dh = DiffLibrary(base, Rep(base, "sound kind=file\n", "sound kind=file edited=1\n"), base);
    const auto     gh = SyncGroupChanges(dh);
    CHECK(gh.size() == 1 && gh[0].kind == SyncRowKind::Edited && gh[0].hand_only);
    CHECK(SyncEditSummary(gh[0]) == "zones edited by hand");
    CHECK(SyncSideSummary(gh[0], false) == "1 edit" && SyncSideSummary(gh[0], true) == "unchanged");
    const auto hl = SyncEditDetailLines(dh, gh[0]);
    CHECK(hl.size() == 1 && hl[0].name == "Zones edited by hand" && hl[0].project == "no \xE2\x86\x92 yes" && hl[0].global == "no");
    std::string full2;
    SyncFooterSummary(SyncSelection().Footer(dh), &full2);

    // the global's side: its own values in the GLOBAL column
    const SyncDiff df = DiffLibrary(base, base, Rep(base, "frame start=0.5 len=0.3", "frame start=0.5 len=0.3 fin=0.02 fout=0.05"));
    const auto     gf = SyncGroupChanges(df);
    CHECK(gf.size() == 1 && gf[0].fades == 1 && SyncEditSummary(gf[0]) == "1 fade");
    CHECK(SyncSideSummary(gf[0], true) == "2 edits" && SyncSideSummary(gf[0], false) == "unchanged");
    const auto fl = SyncEditDetailLines(df, gf[0]);
    CHECK(fl.size() == 3 && fl[1].name == "Fade in" && fl[1].global == "default \xE2\x86\x92 20 ms" && fl[1].project == "default");

    // added / removed variations and a muted one
    std::string m2 = Rep(base, "frame start=3.5 len=0.3\n", "");
    m2 = Rep(m2, "frame start=0.5 len=0.3", "frame start=0.5 len=0.3 on=0");
    const SyncDiff d2 = DiffLibrary(base, m2, Rep(base, "frame start=1.5 len=0.3", "frame start=1.5 len=0.3 db=1"));
    const auto     g2 = SyncGroupChanges(d2);
    CHECK(g2.size() == 1 && g2[0].removed == 1 && g2[0].mutes == 1 && g2[0].gains == 1);
    CHECK(SyncEditSummary(g2[0]) == "1 gain, 1 muted, 1 removed");
    bool removed_line = false;
    for (const SyncDetailLine& l : SyncEditDetailLines(d2, g2[0]))
        if (l.name == "Variation 4 \xC2\xB7 Removed" && l.project == "\xE2\x80\x94" && l.global == "start 3.500 s \xC2\xB7 length 0.300 s") removed_line = true;
    CHECK(removed_line);

    // parts and an opaque value
    const SyncDiff dp = DiffLibrary(base, Rep(base, "sound kind=file\n", "sound kind=file div=G1 divv=V1\n"), base);
    const auto     gp = SyncGroupChanges(dp);
    CHECK(gp.size() == 1 && gp[0].parts == 1 && gp[0].part_fields == 2 && SyncEditSummary(gp[0]) == "2 part settings");
    CHECK(SyncDetailCell(SyncFieldDiff(), true) == "");
    SyncFieldDiff op;
    op.opaque = true;
    op.here_moved = true;
    CHECK(SyncDetailCell(op, false) == "changed" && SyncDetailCell(op, true) == "unchanged");

    // no raw engine words reach the footer: the texts of the changes are sentences
    for (const SyncDiff* x : {&d, &df, &d2, &dp})
        for (const SyncChange& c : x->changes) CHECK(c.text.find("changed (") == std::string::npos && c.text.find("edited changed") == std::string::npos);
}

int main()
{
    const SyncDiff d = Fixture();
    const auto     gs = SyncGroupChanges(d);

    // every change in exactly one group
    size_t total = 0;
    for (const SyncRowGroup& g : gs) total += g.keys.size();
    CHECK(total == d.changes.size());

    // added branch: node + its Sounds + its case + the value only it uses + the group only it uses: ONE row
    const SyncRowGroup* br = GroupOf(gs, "X|rg");
    CHECK(br && br->kind == SyncRowKind::Added && br->keys[0] == "X|rg");
    CHECK(br && br->sounds == 2 && br->value == "Gravel");
    for (const char* k : {"X|s1", "X|s2", "C|sw|Gravel", "VX|g|Gravel"}) CHECK(br && GroupOf(gs, k) == br);
    // the group node Surface and the value Mud are also used by the second case "Mud": not absorbed
    CHECK(GroupOf(gs, "VX|g|Mud") != br);
    CHECK(GroupOf(gs, "GX|g") != br);
    // one row for it: SyncBranchCell and its lines
    CHECK(br && SyncBranchCell(*br, "Random") == "+ Gravel \xC2\xB7 plays Random (2 sounds)");
    CHECK(br && SyncNodeDisplayName(d, "rg") == "Gravel");
    const auto lines = br ? SyncBranchLines(d, *br) : std::vector<std::pair<int, std::string>>();
    CHECK(lines.size() >= 3 && lines[0].second == "Random Gravel" && lines[0].first == 0 && lines[1].second == "FS_Gravel_01" && lines[1].first == 1);

    // ticking the lead's row picks the whole closure (KeepAll on the group's keys)
    {
        SyncSelection sel;
        if (br) sel.KeepAll(d, br->keys, SyncDir::Up);
        CHECK(br && sel.KeepTri(d, br->keys, SyncDir::Up) == SyncTri::All);
        const auto picks = sel.Picks(d);
        CHECK(picks.count("X|s2") && picks.count("C|sw|Gravel") && picks.count("VX|g|Gravel") && picks.count("X|rg"));
        if (br) sel.KeepClear(d, br->keys, SyncDir::Up);
        CHECK(br && sel.KeepTri(d, br->keys, SyncDir::Up) == SyncTri::None);
    }

    // removed branch: node + its case, one row, the sounds read from the text
    const SyncRowGroup* rm = GroupOf(gs, "X|old");
    CHECK(rm && rm->kind == SyncRowKind::Removed && rm->sounds == 3 && rm->value == "Old" && GroupOf(gs, "C|sw|Old") == rm);
    CHECK(rm && SyncBranchCell(*rm, "OldRandom") == "\xE2\x88\x92 Old \xC2\xB7 plays OldRandom (3 sounds)");

    // edited Sound: variations and parts of one Sound, ONE row; its gain stays its own row
    const SyncRowGroup* ed = GroupOf(gs, "V|snd|a");
    CHECK(ed && ed->kind == SyncRowKind::Edited && ed->keys.size() == 4);
    CHECK(ed && ed->edited == 2 && ed->added == 1 && ed->parts == 1);
    CHECK(ed && ed->side == SyncSide::Both);
    CHECK(ed && SyncEditSummary(*ed) == "2 variations edited, 1 added, parts edited");
    CHECK(GroupOf(gs, "F|snd|gain") && GroupOf(gs, "F|snd|gain")->kind == SyncRowKind::Single);
    CHECK(SyncEditLine(*d.Find("V|snd|c")) == "Variation 3: added here");
    CHECK(SyncEditLine(*d.Find("V|snd|b")) == "Variation 2: edited in the global");
    SyncRowGroup one;
    one.kind = SyncRowKind::Edited;
    one.edited = 1;
    CHECK(SyncEditSummary(one) == "1 variation edited");
    one.edited = 0;
    one.parts = 2;
    CHECK(SyncEditSummary(one) == "parts edited");

    // a lone added Sound under an existing node stays a single row
    {
        SyncDiff e;
        e.root = "R";
        Add(&e, "X|a", SyncSide::Here, "R", "A", "present", "absent", {}, false, "a", "Sound A added");
        e.BuildIndex();
        const auto g1 = SyncGroupChanges(e);
        CHECK(g1.size() == 1 && g1[0].kind == SyncRowKind::Single);
    }

    TestEditedSoundWords();

    // names and values
    CHECK(SyncSettingName("Offset") == "Snap offset");
    CHECK(SyncSettingName("Gain") == "Gain");
    CHECK(SyncSettingName("edited changed") == "Edited");
    CHECK(SyncSettingName("fade_in changed") == "Fade in");
    CHECK(SyncSettingValue("Gain", "-3 dB") == "\xE2\x88\x92" "3 dB");
    CHECK(SyncSettingValue("Offset", "12 ms") == "12 ms");
    CHECK(SyncSettingValue("Fades", "100/200 ms") == "in 100 ms \xC2\xB7 out 200 ms");
    CHECK(SyncSettingValue("Fades", "-/200 ms") == "in none \xC2\xB7 out 200 ms");
    CHECK(SyncSettingValue("Fades", "default ms") == "default");
    CHECK(SyncSettingValue("edited changed", "1") == "yes");
    CHECK(SyncSettingValue("Pitch", "") == "not set");
    CHECK(SyncCaseValueName("Switch value Light") == "Light");

    // 1000 changes group in bounded time
    {
        SyncDiff big;
        big.root = "R";
        for (int i = 0; i < 1000; ++i) {
            const std::string u = "n" + std::to_string(i);
            Add(&big, "X|" + u, SyncSide::Here, "R", "N" + std::to_string(i), "present", "absent", {}, true, u, "Random N added");
            Add(&big, "X|" + u + "s", SyncSide::Here, u, "S" + std::to_string(i), "present", "absent", {"X|" + u}, false, u + "s", "Sound added");
        }
        big.BuildIndex();
        const auto t0 = std::chrono::steady_clock::now();
        const auto bg = SyncGroupChanges(big);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(bg.size() == 1000);
        CHECK(ms < 500.0);
    }

    if (g_fail == 0) std::printf("sync_rows_test: all passed\n");
    return g_fail == 0 ? 0 : 1;
}
