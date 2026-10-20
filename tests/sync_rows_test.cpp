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
