// SPDX-License-Identifier: MIT
//
// Host tests of src/library_sync.h (story 13-77b): the I/O matrix of the spec and the engine's rules.

#include "library_sync.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "ravlib.h"
#include "sound_record.h"

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) {                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);             \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

// A Footstep Switch (Concrete -> Walk_Concrete, Wood -> Walk_Wood), each Random holding one Sound.
static const char* kBase =
    "RAVLIB 2 by=A at=10\n"
    "group uid=G1 name=Surface\n"
    "value uid=V1 name=Concrete\n"
    "value uid=V2 name=Wood\n"
    "library uid=L1 name=Foot root=R\n"
    "node uid=R type=switch name=Footstep group=G1\n"
    "case value=V1 child=A\n"
    "case value=V2 child=B\n"
    "node uid=A type=random parent=R name=Walk_Concrete vol=-4\n"
    "node uid=S1 type=sound parent=A name=FS_C_01 offset_ms=12\n"
    "sound kind=file fin=0.001 fout=0.005\n"
    "frame start=0.5 len=0.3\n"
    "frame start=1.5 len=0.3\n"
    "node uid=B type=random parent=R name=Walk_Wood\n"
    "node uid=S2 type=sound parent=B name=FS_W_01\n"
    "sound kind=file\n"
    "frame start=0.25 len=0.2\n";

static std::string Sub(std::string t, const std::string& from, const std::string& to)
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

static const SyncChange* Has(const SyncDiff& d, const std::string& key)
{
    return d.Find(key);
}

static std::map<std::string, SyncDir> All(const SyncDiff& d, SyncDir dir)
{
    std::map<std::string, SyncDir> m;
    for (const SyncChange& c : d.changes) m[c.key] = dir;
    return m;
}

static std::string Base() { return kBase; }

// ---- diff matrix ----------------------------------------------------------------------------------------

static void TestUpToDate()
{
    const SyncDiff d = DiffLibrary(Base(), Base(), Base());
    CHECK(d.changes.empty());
    CHECK(d.had_base);
    CHECK(d.folds.empty());
    CHECK(SyncTagText(CountsOf(d, "A")).empty());
}

static void TestChangedHere()
{
    const std::string m = Sub(Base(), "vol=-4", "vol=-3");
    const SyncDiff    d = DiffLibrary(Base(), m, Base());
    CHECK(d.changes.size() == 1);
    const SyncChange* c = Has(d, "F|A|node|vol");
    CHECK(c && c->side == SyncSide::Here && c->node == "A");
    CHECK(c && c->label == "Gain");
    CHECK(c && c->text == "Gain -4 \xE2\x86\x92 -3 dB");
    CHECK(d.here == 1 && d.file == 0 && d.both == 0);
    CHECK(SyncTagText(CountsOf(d, "A")) == "edited");
    CHECK(SyncRowUpText(TotalCounts(d)) == "\xE2\x86\x91" "1");
    CHECK(SyncRowDownText(TotalCounts(d)).empty());
}

static std::string FileWithMud()
{
    std::string f = Sub(Base(), "value uid=V2 name=Wood\n", "value uid=V2 name=Wood\nvalue uid=V3 name=Mud\n");
    f = Sub(f, "case value=V2 child=B\n", "case value=V2 child=B\ncase value=V3 child=M\n");
    f += "node uid=M type=random parent=R name=Walk_Mud\nnode uid=SM type=sound parent=M name=FS_M_01\nsound kind=file\n";
    return f;
}

static void TestChangedInFile()
{
    const SyncDiff d = DiffLibrary(Base(), Base(), FileWithMud());
    CHECK(d.changes.size() == 4);
    CHECK(d.file == 4 && d.here == 0);
    const SyncChange* cs = Has(d, "C|R|V3");
    CHECK(cs && cs->side == SyncSide::File && cs->node == "R");
    CHECK(cs && cs->text == "Switch value Mud added (plays Walk_Mud)");
    CHECK(cs && cs->needs.size() == 2);  // the Random and the value (the Switch is not added)
    const SyncChange* ss = Has(d, "X|SM");
    CHECK(ss && ss->needs.size() == 1 && ss->needs[0] == "X|M" && ss->node == "M");
    const SyncChange* m = Has(d, "X|M");
    CHECK(m && m->adds_container && m->node == "M" && m->text == "Random Walk_Mud added");
    CHECK(Has(d, "VX|G1|V3") && Has(d, "VX|G1|V3")->text == "Value Mud added");
    CHECK(SyncTagText(CountsOf(d, "R")) == "in file");
}

static void TestBoth()
{
    const std::string m = Sub(Base(), "vol=-4", "vol=-5");
    const std::string f = Sub(Base(), "vol=-4", "vol=-6");
    const SyncDiff    d = DiffLibrary(Base(), m, f);
    CHECK(d.changes.size() == 1 && d.both == 1);
    const SyncChange* c = Has(d, "F|A|node|vol");
    CHECK(c && c->side == SyncSide::Both);
    CHECK(c && c->text == "Gain: here -5 dB \xC2\xB7 file -6 dB");
    CHECK(c && c->here_text == "-5 dB" && c->file_text == "-6 dB");
    CHECK(SyncTagText(CountsOf(d, "A")) == "both");
    CHECK(SyncRowUpText(TotalCounts(d)) == "\xE2\x86\x91" "1" && SyncRowDownText(TotalCounts(d)) == "\xE2\x86\x93" "1");
}

static void TestSameBothSides()
{
    const std::string m = Sub(Base(), "vol=-4", "vol=-5");
    const SyncDiff    d = DiffLibrary(Base(), m, m);
    CHECK(d.changes.empty());
    CHECK(d.folds.size() == 1 && d.folds[0] == "F|A|node|vol");
    SyncResult r;
    CHECK(ApplySync(Base(), m, m, d, {}, &r));
    CHECK(r.base.find("vol=-5") != std::string::npos);  // folded into the base
    CHECK(DiffLibrary(r.base, m, m).folds.empty());
}

static void TestNoBase()
{
    std::string m = Sub(Base(), "vol=-4", "vol=-5");
    m = Sub(m, "offset_ms=12", "offset_ms=18");
    std::string f = Sub(Base(), "name=Walk_Wood", "name=Walk_Oak");
    const SyncDiff d = DiffLibrary("", m, f);
    CHECK(!d.had_base);
    CHECK(d.changes.size() == 3);
    CHECK(d.both == 3 && d.here == 0 && d.file == 0);
    for (const SyncChange& c : d.changes) CHECK(c.needs.empty());
}

static void TestRemovedVsEdited()
{
    // here removes Walk_Wood (and its case), the file edits the Sound inside
    std::string m = Base();
    m = Sub(m, "case value=V2 child=B\n", "");
    m = Sub(m, "node uid=B type=random parent=R name=Walk_Wood\nnode uid=S2 type=sound parent=B name=FS_W_01\nsound kind=file\nframe start=0.25 len=0.2\n", "");
    std::string f = Sub(Base(), "name=FS_W_01", "name=FS_W_01 offset_ms=3");
    f = Sub(f, "frame start=0.25 len=0.2", "frame start=0.25 len=0.4");
    const SyncDiff d = DiffLibrary(Base(), m, f);
    const SyncChange* x = Has(d, "X|B");
    CHECK(x && x->side == SyncSide::Both);
    CHECK(x && x->here_text == "removed" && x->file_text == "2 changes");
    CHECK(!Has(d, "X|S2"));  // its subtree goes with it, never listed separately
    CHECK(!Has(d, "F|S2|node|offset_ms"));
    CHECK(Has(d, "C|R|V2") && Has(d, "C|R|V2")->side == SyncSide::Here);
    // removed with nothing edited: a plain Here removal, and a Random's text says its sounds
    const SyncDiff d2 = DiffLibrary(Base(), m, Base());
    CHECK(Has(d2, "X|B") && Has(d2, "X|B")->side == SyncSide::Here);
    CHECK(Has(d2, "X|B")->text == "Random Walk_Wood removed (1 sound)");
    // the Case removal is needed by the removal
    CHECK(Has(d2, "X|B")->needs.size() == 1 && Has(d2, "X|B")->needs[0] == "C|R|V2");
}

static void TestAdoptPullsNeeds()
{
    std::string m = FileWithMud();  // the same additions, made HERE this time
    const SyncDiff d = DiffLibrary(Base(), m, Base());
    CHECK(d.here == 4);
    std::map<std::string, SyncDir> pick = {{"X|SM", SyncDir::Up}};
    const auto closed = CloseSelection(d, pick);
    CHECK(closed.count("X|M") && closed.at("X|M") == SyncDir::Up);
    CHECK(closed.size() == 2);
    // adopting the Case pulls the value, the Random (and, through it, nothing else)
    const auto c2 = CloseSelection(d, {{"C|R|V3", SyncDir::Up}});
    CHECK(c2.count("VX|G1|V3") && c2.count("X|M") && c2.size() == 3);
}

static void TestDropPullsDependents()
{
    std::string m = FileWithMud();
    m += "node uid=SM2 type=sound parent=M name=FS_M_02\nsound kind=file\nnode uid=SM3 type=sound parent=M name=FS_M_03\nsound kind=file\n";
    const SyncDiff d = DiffLibrary(Base(), m, Base());
    const auto     closed = CloseSelection(d, {{"X|M", SyncDir::Down}});
    CHECK(closed.count("X|SM") && closed.count("X|SM2") && closed.count("X|SM3") && closed.count("C|R|V3"));
    CHECK(!closed.count("VX|G1|V3"));  // the value does not need the Random
    // a Both change pulls nothing
    const std::string b1 = Sub(Base(), "vol=-4", "vol=-5"), b2 = Sub(Base(), "vol=-4", "vol=-6");
    const SyncDiff    db = DiffLibrary(Base(), b1, b2);
    CHECK(CloseSelection(db, {{"F|A|node|vol", SyncDir::Up}}).size() == 1);
}

static void TestSendMineOfContainer()
{
    // Walk_Concrete: one here change (Gain), one both change (the Sound's offset)
    std::string m = Sub(Base(), "vol=-4", "vol=-3");
    m = Sub(m, "offset_ms=12", "offset_ms=18");
    std::string f = Sub(Base(), "offset_ms=12", "offset_ms=20");
    const SyncDiff d = DiffLibrary(Base(), m, f);
    CHECK(d.here == 1 && d.both == 1);
    const std::vector<std::string> keys = ContainerChangeKeys(d, "A");
    CHECK(keys.size() == 2);
    std::map<std::string, SyncDir> picks;
    for (const std::string& k : keys) picks[k] = SyncDir::Up;
    SyncResult r;
    CHECK(ApplySync(Base(), m, f, d, CloseSelection(d, picks), &r));
    CHECK(r.sent == 2 && r.taken == 0);
    CHECK(r.file_changed && !r.mine_changed);
    CHECK(r.file.find("vol=-3") != std::string::npos && r.file.find("offset_ms=18") != std::string::npos);
    CHECK(DiffLibrary(r.base, r.mine, r.file).changes.empty());
    // the rest of the file stays: only those two lines moved
    const SyncDiff after = DiffLibrary(f, f, r.file);  // file before vs after
    CHECK(after.changes.size() == 2);
}

static void TestGetFiles()
{
    // Walk_Wood: three file changes (a Gain, a rename, a variation)
    std::string f = Sub(Base(), "name=Walk_Wood", "name=Walk_Wood vol=-2");
    f = Sub(f, "name=FS_W_01", "name=FS_Wood_01");
    f = Sub(f, "frame start=0.25 len=0.2\n", "frame start=0.25 len=0.2\nframe start=2 len=0.1\n");
    const SyncDiff d = DiffLibrary(Base(), Base(), f);
    CHECK(d.file == 3);
    CHECK(Has(d, "F|S2|node|name")->text == "Renamed FS_W_01 \xE2\x86\x92 FS_Wood_01");
    CHECK(Has(d, "V|S2|2000")->text == "Variation 2 of FS_W_01 added");
    SyncResult r;
    CHECK(ApplySync(Base(), Base(), f, d, All(d, SyncDir::Down), &r));
    CHECK(r.taken == 3 && r.sent == 0);
    CHECK(!r.file_changed && r.mine_changed);  // the file is untouched, no .bak needed
    CHECK(r.mine.find("vol=-2") != std::string::npos && r.mine.find("FS_Wood_01") != std::string::npos);
    CHECK(DiffLibrary(r.base, r.mine, r.file).changes.empty());
}

static void TestGroupValueNeverRemoved()
{
    // the file lacks Concrete: not a change
    std::string f = Sub(Base(), "value uid=V1 name=Concrete\n", "");
    const SyncDiff d = DiffLibrary(Base(), Base(), f);
    CHECK(d.changes.empty());
    // a value added here and dropped (Down): the project keeps it, nothing is listed after
    const std::string m = Sub(Base(), "value uid=V2 name=Wood\n", "value uid=V2 name=Wood\nvalue uid=V3 name=Mud\n");
    const SyncDiff    d2 = DiffLibrary(Base(), m, Base());
    CHECK(d2.here == 1 && Has(d2, "VX|G1|V3"));
    SyncResult r;
    CHECK(ApplySync(Base(), m, Base(), d2, {{"VX|G1|V3", SyncDir::Down}}, &r));
    CHECK(r.mine.find("name=Mud") != std::string::npos);
    CHECK(r.file.find("name=Mud") == std::string::npos);
    CHECK(DiffLibrary(r.base, r.mine, r.file).changes.empty());
    // sent Up: the file gets it
    SyncResult r2;
    CHECK(ApplySync(Base(), m, Base(), d2, {{"VX|G1|V3", SyncDir::Up}}, &r2));
    CHECK(r2.file.find("name=Mud") != std::string::npos);
    // a value added in the file arrives in the project group, additively
    const SyncDiff d3 = DiffLibrary(Base(), Base(), m);
    SyncResult     r3;
    CHECK(ApplySync(Base(), Base(), m, d3, All(d3, SyncDir::Down), &r3));
    CHECK(r3.mine.find("name=Mud") != std::string::npos && r3.mine.find("name=Concrete") != std::string::npos);
}

static void TestChildOrder()
{
    // the file reorders Walk_Wood before Walk_Concrete
    std::string f = "RAVLIB 2 by=B at=20\n";
    std::string b = Base();
    const size_t a = b.find("node uid=A"), bb = b.find("node uid=B");
    std::string  head = b.substr(b.find('\n') + 1, a - (b.find('\n') + 1));
    std::string  blockA = b.substr(a, bb - a), blockB = b.substr(bb);
    f += head + blockB + blockA;
    const SyncDiff d = DiffLibrary(Base(), Base(), f);
    const SyncChange* o = Has(d, "O|R");
    CHECK(o && o->side == SyncSide::File && o->text == "Order of children changed");
    CHECK(d.changes.size() == 1);
    SyncResult r;
    CHECK(ApplySync(Base(), Base(), f, d, All(d, SyncDir::Down), &r));
    CHECK(r.mine.find("node uid=B") < r.mine.find("node uid=A"));
    CHECK(DiffLibrary(r.base, r.mine, r.file).changes.empty());
    // an addition is not an order change
    const SyncDiff d2 = DiffLibrary(Base(), Base(), FileWithMud());
    CHECK(!Has(d2, "O|R"));
}

static void TestVariationsByStartTime()
{
    // the file's variation starts 0.8 ms later: the same variation; edited length: a change; a 1.5 s one: new
    std::string f = Sub(Base(), "frame start=0.5 len=0.3", "frame start=0.5008 len=0.3");
    CHECK(DiffLibrary(Base(), Base(), f).changes.empty());
    f = Sub(Base(), "frame start=0.5 len=0.3", "frame start=0.5008 len=0.31 on=0");
    const SyncDiff d = DiffLibrary(Base(), Base(), f);
    CHECK(d.changes.size() == 1 && Has(d, "V|S1|500") && d.changes[0].text == "Variation 1 of FS_C_01 changed");
    SyncResult r;
    CHECK(ApplySync(Base(), Base(), f, d, All(d, SyncDir::Down), &r));
    CHECK(r.mine.find("on=0") != std::string::npos);
    CHECK(DiffLibrary(r.base, r.mine, r.file).changes.empty());
    // removed here
    std::string m = Sub(Base(), "frame start=1.5 len=0.3\n", "");
    const SyncDiff d2 = DiffLibrary(Base(), m, Base());
    CHECK(d2.changes.size() == 1 && d2.changes[0].text == "Variation 2 of FS_C_01 removed");
    SyncResult r2;
    CHECK(ApplySync(Base(), m, Base(), d2, All(d2, SyncDir::Up), &r2));
    CHECK(r2.file.find("start=1.5") == std::string::npos);
}

static void TestAddedContainerUnderContainer()
{
    // the file adds Random X under Walk_Concrete with a Sound under X
    std::string f = Base();
    f += "node uid=X type=random parent=A name=Inner\nnode uid=SX type=sound parent=X name=FS_X\nsound kind=file\n";
    const SyncDiff d = DiffLibrary(Base(), Base(), f);
    CHECK(d.changes.size() == 2);
    CHECK(Has(d, "X|X") && Has(d, "X|X")->node == "X" && Has(d, "X|X")->adds_container);
    CHECK(Has(d, "X|X")->needs.empty());  // Walk_Concrete exists
    CHECK(Has(d, "X|SX")->needs.size() == 1);
    // Get file's of Walk_Concrete (the container's gesture) takes what sits under X, which has no row here
    const auto keys = ContainerChangeKeys(d, "A");
    CHECK(keys.size() == 2);
    const auto keysR = ContainerChangeKeys(d, "R");
    CHECK(keysR.empty());  // Walk_Concrete has a row of its own: they belong to it
    const auto keysB = ContainerChangeKeys(d, "B");
    CHECK(keysB.empty());
    // apply: the Sound is picked alone, its Random comes along
    const auto closed = CloseSelection(d, {{"X|SX", SyncDir::Down}});
    CHECK(closed.size() == 2);
    SyncResult r;
    CHECK(ApplySync(Base(), Base(), f, d, closed, &r));
    CHECK(r.mine.find("node uid=X ") != std::string::npos && r.mine.find("node uid=SX ") != std::string::npos);
    CHECK(r.mine.find("node uid=X ") < r.mine.find("node uid=SX "));
    CHECK(DiffLibrary(r.base, r.mine, r.file).changes.empty());
    // dropping the Random (here added, Down) removes its Sound first
    std::string m = f;
    const SyncDiff dh = DiffLibrary(Base(), m, Base());
    SyncResult     rh;
    CHECK(ApplySync(Base(), m, Base(), dh, CloseSelection(dh, {{"X|X", SyncDir::Down}}), &rh));
    CHECK(rh.mine == Base() || DiffLibrary(rh.base, rh.mine, rh.file).changes.empty());
    CHECK(rh.mine.find("node uid=X ") == std::string::npos && rh.mine.find("node uid=SX ") == std::string::npos);
}

static void TestApplyThroughSwitchValues()
{
    // Get file's on the Switch: the value, the Random and its Sound arrive together
    const SyncDiff d = DiffLibrary(Base(), Base(), FileWithMud());
    SyncResult     r;
    CHECK(ApplySync(Base(), Base(), FileWithMud(), d, CloseSelection(d, {{"C|R|V3", SyncDir::Down}, {"X|SM", SyncDir::Down}}), &r));
    CHECK(r.taken == 4 && !r.file_changed);
    CHECK(r.mine.find("value uid=V3 name=Mud") != std::string::npos);
    CHECK(r.mine.find("case value=V3 child=M") != std::string::npos);
    CHECK(r.mine.find("node uid=SM ") != std::string::npos);
    CHECK(DiffLibrary(r.base, r.mine, r.file).changes.empty());
}

static void TestRoundTripPartial()
{
    // three changes (here Gain, file rename, file Wood gain); pick two, the third stays
    std::string m = Sub(Base(), "vol=-4", "vol=-3");
    std::string f = Sub(Base(), "name=FS_W_01", "name=FS_Wood_01");
    f = Sub(f, "name=Walk_Wood", "name=Walk_Wood vol=-2");
    const SyncDiff d = DiffLibrary(Base(), m, f);
    CHECK(d.changes.size() == 3);
    SyncResult r;
    CHECK(ApplySync(Base(), m, f, d, {{"F|A|node|vol", SyncDir::Up}, {"F|S2|node|name", SyncDir::Down}}, &r));
    const SyncDiff left = DiffLibrary(r.base, r.mine, r.file);
    CHECK(left.changes.size() == 1);
    CHECK(Has(left, "F|B|node|vol") && Has(left, "F|B|node|vol")->side == SyncSide::File);
    // a dropped one-sided change reverts its side: here Gain dropped = project back to -4
    SyncResult r2;
    CHECK(ApplySync(Base(), m, f, d, {{"F|A|node|vol", SyncDir::Down}}, &r2));
    CHECK(r2.mine.find("vol=-4") != std::string::npos && r2.sent == 0 && r2.taken == 1);
    const SyncDiff left2 = DiffLibrary(r2.base, r2.mine, r2.file);
    CHECK(left2.changes.size() == 2 && !Has(left2, "F|A|node|vol"));
}

static void TestApplyOrdering()
{
    // removal children first, addition parents first, in one Apply
    std::string m = Base();
    m += "node uid=N1 type=random parent=B name=New1\nnode uid=N2 type=sound parent=N1 name=New2\nsound kind=file\n";
    m = Sub(m, "node uid=S1 type=sound parent=A name=FS_C_01 offset_ms=12\nsound kind=file fin=0.001 fout=0.005\nframe start=0.5 len=0.3\nframe start=1.5 len=0.3\n", "");
    m = Sub(m, "case value=V1 child=A\n", "");
    m = Sub(m, "node uid=A type=random parent=R name=Walk_Concrete vol=-4\n", "");
    const SyncDiff d = DiffLibrary(Base(), m, Base());
    SyncResult     r;
    CHECK(ApplySync(Base(), m, Base(), d, CloseSelection(d, All(d, SyncDir::Up)), &r));
    CHECK(r.file.find("node uid=A ") == std::string::npos && r.file.find("node uid=S1 ") == std::string::npos);
    CHECK(r.file.find("node uid=N1 ") != std::string::npos && r.file.find("node uid=N2 ") != std::string::npos);
    CHECK(r.file.find("node uid=N1 ") < r.file.find("node uid=N2 "));
    CHECK(DiffLibrary(r.base, r.mine, r.file).changes.empty());
}

static void TestAdaptationsAreNotFileChanges()
{
    // the merge renamed the library and replaced the Switch's default on a merged group: base == mine != file
    std::string file = Sub(Base(), "name=Foot", "name=Foot2");
    std::string mine = Base();
    const SyncDiff d = DiffLibrary(Base(), mine, file, true);  // the file's by / at are the base's
    CHECK(d.changes.empty());
    // someone else wrote the file since: now its name is a file change
    const SyncDiff d2 = DiffLibrary(Base(), mine, file, false);
    CHECK(d2.changes.size() == 1 && d2.changes[0].side == SyncSide::File);
    // a here edit still reads as here, the adaptation is not written to the file
    const std::string m3 = Sub(Base(), "vol=-4", "vol=-3");
    const SyncDiff    d3 = DiffLibrary(Base(), m3, file, true);
    CHECK(d3.changes.size() == 1 && d3.changes[0].side == SyncSide::Here);
    SyncResult r;
    CHECK(ApplySync(Base(), m3, file, d3, All(d3, SyncDir::Up), &r));
    CHECK(r.file.find("name=Foot2") != std::string::npos && r.file.find("vol=-3") != std::string::npos);
}

static void TestUidTranslation()
{
    const std::vector<std::pair<std::string, std::string>> map = {{"G1", "G7"}, {"V1", "V9"}};
    const std::string file = Base();
    const std::string proj = TranslateUidsText(file, map, true);
    CHECK(proj.find("group uid=G7") != std::string::npos);
    CHECK(proj.find("value uid=V9 name=Concrete") != std::string::npos);
    CHECK(proj.find("case value=V9 child=A") != std::string::npos);
    CHECK(proj.find("group=G7") != std::string::npos);
    CHECK(proj.find("RAVLIB 2 by=A at=10") == 0);
    CHECK(TranslateUidsText(proj, map, false) == file);
    // lists and role maps
    const std::string lst = "RAVLIB 2\nsound kind=file divv=V1,V2 rmap=heel:V1,toe:V2\n";
    CHECK(TranslateUidsText(lst, map, true) == "RAVLIB 2\nsound kind=file divv=V9,V2 rmap=heel:V9,toe:V2\n");
    // a diff in project space: a file value changes under the renewed uid
    const std::string f2 = Sub(file, "name=Concrete", "name=Hard");
    const SyncDiff    d = DiffLibrary(TranslateUidsText(file, map, true), TranslateUidsText(file, map, true), TranslateUidsText(f2, map, true));
    CHECK(d.changes.size() == 1 && d.changes[0].key == "VF|G7|V9|name");
}

static void TestNoCommonVersionApply()
{
    std::string m = Sub(Base(), "vol=-4", "vol=-5");
    std::string f = Sub(Base(), "name=Walk_Wood", "name=Walk_Oak");
    const SyncDiff d = DiffLibrary("", m, f);
    SyncResult     r;
    CHECK(ApplySync("", m, f, d, {{"F|A|node|vol", SyncDir::Up}}, &r));
    CHECK(r.base.empty());  // still no common version: the rest reads as both
    CHECK(DiffLibrary(r.base, r.mine, r.file).changes.size() == 1);
    SyncResult r2;
    CHECK(ApplySync("", m, f, d, All(d, SyncDir::Down), &r2));
    CHECK(!r2.base.empty());  // everything settled: the common version is the result
    CHECK(DiffLibrary(r2.base, r2.mine, r2.file).changes.empty());
}

// ---- gates -----------------------------------------------------------------------------------------------

static void TestApplyGate()
{
    SyncFileStamp a;
    a.size = 100;
    a.mtime = 5;
    SyncFileStamp b = a;
    CHECK(DecideApplyGate(a, b) == ApplyGate::Go);
    b.mtime = 6;
    CHECK(DecideApplyGate(a, b) == ApplyGate::FileChanged);
    b = a;
    b.size = 101;
    CHECK(DecideApplyGate(a, b) == ApplyGate::FileChanged);
    b = a;
    b.size = -1;
    CHECK(DecideApplyGate(a, b) == ApplyGate::FileGone);
    // same stat, other content (a write inside the same timestamp)
    a.hash = SyncTextHash("x");
    b = a;
    b.hash = SyncTextHash("y");
    CHECK(DecideApplyGate(a, b) == ApplyGate::FileChanged);
}

static void TestJournal()
{
    SyncJournal j;
    j.lib_uid = "L1";
    j.file = "Foot.ravlib";
    j.before = "old";
    j.after = "new";
    j.base_before = SyncTextHash("b0");
    j.base_after = SyncTextHash("b1");
    // right after the Apply: nothing to do
    CHECK(DecideJournal(j, j.base_after, true, SyncTextHash("new")) == JournalStep::None);
    // Ctrl+Z, nobody wrote the file
    CHECK(DecideJournal(j, j.base_before, true, SyncTextHash("new")) == JournalStep::RestoreBefore);
    // Ctrl+Z, another project wrote it
    CHECK(DecideJournal(j, j.base_before, true, SyncTextHash("other")) == JournalStep::FileWrittenSince);
    // Ctrl+Z, the file is gone
    CHECK(DecideJournal(j, j.base_before, false, 0) == JournalStep::FileWrittenSince);
    // after the restore the file is "old": nothing more; redo brings it back
    CHECK(DecideJournal(j, j.base_before, true, SyncTextHash("old")) == JournalStep::None);
    CHECK(DecideJournal(j, j.base_after, true, SyncTextHash("old")) == JournalStep::RestoreAfter);
    CHECK(DecideJournal(j, j.base_after, true, SyncTextHash("other")) == JournalStep::None);
    // an unrelated base
    CHECK(DecideJournal(j, SyncTextHash("zz"), true, SyncTextHash("new")) == JournalStep::None);
}

// ---- local fields (a real store) --------------------------------------------------------------------------

static void TestLocalFieldsAreNotChanges()
{
    SoundStore s;
    SoundLibrary l;
    l.uid = "L1";
    l.name = "Foot";
    l.root = "R";
    l.ravlib_path = "Foot.ravlib";
    SoundNode root;
    root.uid = "R";
    root.type = SoundNodeType::Random;
    root.name = "Root";
    SoundNode snd;
    snd.uid = "S";
    snd.parent = "R";
    snd.type = SoundNodeType::Sound;
    snd.name = "FS";
    snd.sound.source.kind = SoundSourceKind::File;
    snd.sound.source.item_guid = "{AAAA}";
    l.nodes = {root, snd};
    s.libraries.push_back(l);
    const std::string mine = SerializeRavlib(RavlibOf(s, "L1", nullptr));
    s.libraries[0].nodes[1].sound.source.item_guid = "{BBBB}";
    s.libraries[0].ravlib_path = "Other.ravlib";
    s.libraries[0].kit_offered = true;
    const std::string file = SerializeRavlib(RavlibOf(s, "L1", nullptr), "B", 99);
    CHECK(DiffLibrary(mine, mine, file).changes.empty());
}

static void TestTags()
{
    LibSyncCounts c;
    CHECK(SyncTagText(c).empty() && SyncTagTip(c).empty());
    c.here = 2;
    CHECK(SyncTagText(c) == "edited \xC2\xB7 2");
    CHECK(SyncTagTip(c) == "Changed here, not in the file yet: 2 changes. Click to see what");
    c.here = 0;
    c.file = 1;
    CHECK(SyncTagText(c) == "in file");
    CHECK(SyncTagTip(c) == "Changed in the file since this project last synced: 1 change. Click to see what");
    c.here = 1;
    CHECK(SyncTagText(c) == "both \xC2\xB7 2");
    c.both = 1;
    CHECK(SyncRowTip(c).find("1 change made here, not in the file") == 0);
    CHECK(SyncRowTip(c).find("Click to open Sync with file") != std::string::npos);
    CHECK(SyncAppliedToast(2, 1, "Foot.ravlib") ==
          "Applied: 2 written to Foot.ravlib (the version before is kept as Foot.ravlib.bak), 1 taken here");
    CHECK(SyncAppliedToast(0, 3, "Foot.ravlib") == "Applied: 3 taken here");
}

// ---- a real store: Add (merge), edit, Apply, put back ----------------------------------------------------

static SoundNode MkNode(const std::string& uid, const std::string& parent, SoundNodeType t, const std::string& name)
{
    SoundNode n;
    n.uid = uid;
    n.parent = parent;
    n.type = t;
    n.name = name;
    return n;
}

static SoundStore MkFileStore()
{
    SoundStore s;
    SwitchGroup g;
    g.uid = "G1";
    g.name = "Surface";
    g.values = {SwitchValue{"V1", "Concrete", {}, {}}, SwitchValue{"V2", "Wood", {}, {}}};
    s.groups.push_back(g);
    SoundLibrary l;
    l.uid = "L1";
    l.name = "Foot";
    l.root = "R";
    SoundNode r = MkNode("R", "", SoundNodeType::Switch, "Footstep");
    r.group_uid = "G1";
    r.cases = {SwitchCase{"V1", "A", {}}, SwitchCase{"V2", "B", {}}};
    SoundNode a = MkNode("A", "R", SoundNodeType::Random, "Walk_Concrete");
    a.volume_db = -4;
    SoundNode s1 = MkNode("S1", "A", SoundNodeType::Sound, "FS_C_01");
    s1.sound.source.kind = SoundSourceKind::File;
    SoundNode b = MkNode("B", "R", SoundNodeType::Random, "Walk_Wood");
    l.nodes = {r, a, s1, b};
    s.libraries.push_back(l);
    return s;
}

static void TestRealStoreAddEditApply()
{
    const SoundStore fstore = MkFileStore();
    const std::string file_text = SerializeRavlib(fstore, "ProjA.rpp", 100);
    // Project B has its own "Surface" group (other uids, lower-case names, other roles) and a library named Foot
    SoundStore b;
    SwitchGroup bg;
    bg.uid = "GB";
    bg.name = "surface";
    bg.values = {SwitchValue{"VB1", "concrete", {}, {}}, SwitchValue{"VB9", "Metal", {}, {}}};
    b.groups.push_back(bg);
    SoundLibrary other;
    other.uid = "LX";
    other.name = "Foot";
    other.root = "RX";
    other.nodes = {MkNode("RX", "", SoundNodeType::Random, "Other")};
    b.libraries.push_back(other);
    SoundStore parsed;
    CHECK(ParseRavlib(file_text, &parsed));
    std::string uid, why;
    std::vector<std::pair<std::string, std::string>> map;
    LinkBase base;
    CHECK(AddRavlibLibrary(b, parsed, "Foot.ravlib", &uid, &why, &map, &base));
    CHECK(!map.empty());
    CHECK(base.file_by == "ProjA.rpp" && base.file_at == 100);
    const std::string mine = SerializeRavlib(RavlibOf(b, uid, nullptr));
    CHECK(mine == base.text);
    const std::string proj_file = TranslateUidsText(file_text, map, true);
    // the merge's own adaptations (name "Foot 2", the project's group name / values) are not file changes while the file
    // is as it was
    const SyncDiff quiet = DiffLibrary(base.text, mine, proj_file, true);
    CHECK(quiet.changes.empty());
    CHECK(TranslateUidsText(proj_file, map, false) == file_text);
    // edit here: a gain; the file is untouched: one Here change
    SoundStore edited = b;
    FindNode(*FindLibrary(edited, uid), "A")->volume_db = -1;
    const std::string mine2 = SerializeRavlib(RavlibOf(edited, uid, nullptr));
    const SyncDiff    d = DiffLibrary(base.text, mine2, proj_file, true);
    CHECK(d.changes.size() == 1 && d.changes[0].side == SyncSide::Here);
    SyncResult r;
    CHECK(ApplySync(base.text, mine2, proj_file, d, All(d, SyncDir::Up), &r));
    CHECK(r.file_changed && !r.mine_changed);
    // what the worker writes: back to the file's uids, still the file's own group and library names
    const std::string back = TranslateUidsText(r.file, map, false);
    SoundStore        wrote;
    CHECK(ParseRavlib(back, &wrote));
    CHECK(wrote.libraries[0].name == "Foot" && wrote.groups[0].name == "Surface" && wrote.groups[0].uid == "G1");
    CHECK(FindNode(wrote.libraries[0], "A")->volume_db == -1);
    // put the merged text back into the store: stable, and the base matches it
    SoundStore merged;
    CHECK(ParseRavlib(r.mine, &merged));
    CHECK(ReplaceLibraryFromMerge(edited, uid, merged));
    CHECK(SerializeRavlib(RavlibOf(edited, uid, nullptr)) == r.mine);
    CHECK(DiffLibrary(r.base, r.mine, r.file, true).changes.empty());  // the file's header is the marker now
    const SyncDiff raw = DiffLibrary(r.base, r.mine, r.file, false);   // no marker: only the adaptations read as file changes
    CHECK(raw.here == 0 && raw.both == 0);
    CHECK(FindLibrary(edited, uid)->ravlib_path == "Foot.ravlib");
    CHECK(edited.groups.size() == 1);  // merged into the project's own group, nothing added or removed
}

static void TestReplaceKeepsLocalFields()
{
    SoundStore st = MkFileStore();
    SoundLibrary& l = st.libraries[0];
    l.ravlib_path = "Foot.ravlib";
    l.kit_offered = true;
    l.nodes[2].sound.source.item_guid = "{GUID-1}";
    SoundStore merged = MkFileStore();
    merged.libraries[0].nodes[1].volume_db = -9;
    merged.libraries[0].nodes[2].sound.source.item_guid.clear();
    merged.groups[0].values.push_back(SwitchValue{"V3", "Mud", {}, {}});
    merged.groups[0].name = "Surfaces";
    CHECK(ReplaceLibraryFromMerge(st, "L1", merged));
    const SoundLibrary& r = st.libraries[0];
    CHECK(r.ravlib_path == "Foot.ravlib" && r.kit_offered);
    CHECK(r.nodes[2].sound.source.item_guid == "{GUID-1}");
    CHECK(r.nodes[1].volume_db == -9);
    CHECK(st.groups[0].values.size() == 3 && st.groups[0].name == "Surfaces");
    // a group / value the merged text lacks is kept
    SoundStore fewer = MkFileStore();
    fewer.groups[0].values.erase(fewer.groups[0].values.begin());
    CHECK(ReplaceLibraryFromMerge(st, "L1", fewer));
    CHECK(st.groups[0].values.size() == 3);
    CHECK(!ReplaceLibraryFromMerge(st, "nope", fewer));
}

int main()
{
    TestUpToDate();
    TestChangedHere();
    TestChangedInFile();
    TestBoth();
    TestSameBothSides();
    TestNoBase();
    TestRemovedVsEdited();
    TestAdoptPullsNeeds();
    TestDropPullsDependents();
    TestSendMineOfContainer();
    TestGetFiles();
    TestGroupValueNeverRemoved();
    TestChildOrder();
    TestVariationsByStartTime();
    TestAddedContainerUnderContainer();
    TestApplyThroughSwitchValues();
    TestRoundTripPartial();
    TestApplyOrdering();
    TestAdaptationsAreNotFileChanges();
    TestUidTranslation();
    TestNoCommonVersionApply();
    TestApplyGate();
    TestJournal();
    TestLocalFieldsAreNotChanges();
    TestTags();
    TestRealStoreAddEditApply();
    TestReplaceKeepsLocalFields();
    if (g_fail == 0) std::printf("library_sync_test: all passed\n");
    return g_fail == 0 ? 0 : 1;
}
