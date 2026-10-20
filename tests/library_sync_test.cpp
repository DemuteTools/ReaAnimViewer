// SPDX-License-Identifier: MIT
//
// Host tests of src/library_sync.h (story 13-77b): the I/O matrix of the spec and the engine's rules.

#include "library_sync.h"

#include <algorithm>
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
    // review H5: the root's row holds its Case, the Random new in the file (no row of its own yet), its Sound and the
    // group value: tag, bar and gesture agree
    CHECK(SyncTagText(CountsOf(d, "R")) == "in file \xC2\xB7 4");
    CHECK(ContainerChangeKeys(d, "R").size() == 4);
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
    // Review 13-77b H3: the merge renamed the library on a clash. The base (merged copy) says Foot, the file base Foot2.
    const std::string file = Sub(Base(), "name=Foot", "name=Foot2");
    const std::string mine = Base();
    CHECK(DiffLibrary(Base(), file, mine, file).changes.empty());
    // someone else wrote the file since, for another reason: the adaptation still never reads as a file change
    const std::string file2 = Sub(file, "vol=-4", "vol=-6");
    const SyncDiff    d2 = DiffLibrary(Base(), file, mine, file2);
    CHECK(d2.changes.size() == 1 && Has(d2, "F|A|node|vol") && Has(d2, "F|A|node|vol")->side == SyncSide::File);
    CHECK(!Has(d2, "LF|name"));
    // a hand edit of the name in the file is seen exactly (no clock): a file change from the file's own name
    const std::string file3 = Sub(Base(), "name=Foot", "name=Footsteps");
    const SyncDiff    d3n = DiffLibrary(Base(), file, mine, file3);
    CHECK(Has(d3n, "LF|name") && Has(d3n, "LF|name")->side == SyncSide::File);
    CHECK(Has(d3n, "LF|name") && Has(d3n, "LF|name")->text == "Renamed library Foot2 \xE2\x86\x92 Footsteps");
    // a here edit reads as here; the root's Send mine never writes the project's name into the shared file
    const std::string m3 = Sub(Base(), "vol=-4", "vol=-3");
    const SyncDiff    d3 = DiffLibrary(Base(), file, m3, file);
    CHECK(d3.changes.size() == 1 && d3.changes[0].side == SyncSide::Here);
    CHECK(ContainerChangeKeys(d3, "A").size() == 1);
    SyncResult r;
    CHECK(ApplySync(Base(), file, m3, file, d3, All(d3, SyncDir::Up), &r));
    CHECK(r.file.find("name=Foot2") != std::string::npos && r.file.find("vol=-3") != std::string::npos);
    CHECK(r.base.find("name=Foot ") != std::string::npos);  // the project's own name stays in the base
    CHECK(r.file_base == r.file);
    CHECK(DiffLibrary(r.base, r.file_base, r.mine, r.file).changes.empty());
    // ... and Get file's never takes the file's name over the project's: nothing listed, nothing taken
    const SyncDiff d4 = DiffLibrary(Base(), file, mine, file2);
    SyncResult     r4;
    CHECK(ApplySync(Base(), file, mine, file2, d4, All(d4, SyncDir::Down), &r4));
    CHECK(r4.mine.find("name=Foot ") != std::string::npos && r4.mine.find("vol=-6") != std::string::npos);
    // probe P5: here -5, file -9 (base -4): both, never a silent Send mine over the file's edit
    const SyncDiff d5 = DiffLibrary(Base(), Base(), Sub(Base(), "vol=-4", "vol=-5"), Sub(Base(), "vol=-4", "vol=-9"));
    CHECK(d5.changes.size() == 1 && d5.changes[0].side == SyncSide::Both);
    // a project saved before the file base existed: unknown, every difference reads as both (never as untouched)
    const SyncDiff d6 = DiffLibrary(Base(), "", m3, file);
    CHECK(!d6.had_base && d6.both == 2 && d6.here == 0 && d6.file == 0);
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
    const SyncDiff quiet = DiffLibrary(base.text, proj_file, mine, proj_file);
    CHECK(quiet.changes.empty());
    CHECK(TranslateUidsText(proj_file, map, false) == file_text);
    // edit here: a gain; the file is untouched: one Here change
    SoundStore edited = b;
    FindNode(*FindLibrary(edited, uid), "A")->volume_db = -1;
    const std::string mine2 = SerializeRavlib(RavlibOf(edited, uid, nullptr));
    const SyncDiff    d = DiffLibrary(base.text, proj_file, mine2, proj_file);
    CHECK(d.changes.size() == 1 && d.changes[0].side == SyncSide::Here);
    SyncResult r;
    CHECK(ApplySync(base.text, proj_file, mine2, proj_file, d, All(d, SyncDir::Up), &r));
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
    CHECK(DiffLibrary(r.base, r.file_base, r.mine, r.file).changes.empty());  // the two common versions
    const SyncDiff raw = DiffLibrary(r.base, r.mine, r.file);  // file base = base: only the adaptations differ
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

// ---- review 13-77b ---------------------------------------------------------------------------------------

static bool In(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

static std::map<std::string, SyncDir> PickSubject(const SyncDiff& d, const std::string& s, SyncDir dir)
{
    std::map<std::string, SyncDir> m;
    for (const SyncChange& c : d.changes)
        if (c.subject == s) m[c.key] = dir;
    return CloseSelection(d, m);
}

static std::map<std::string, SyncDir> PickRow(const SyncDiff& d, const std::string& row, SyncDir dir)
{
    std::map<std::string, SyncDir> m;
    for (const std::string& k : ContainerChangeKeys(d, row)) m[k] = dir;
    return CloseSelection(d, m);
}

static void TestMoveIntoNewContainer()
{
    // H1 / probe P1: here, a new Random N and FS_C_01 moved into it; Send mine on the Sound's own panel
    std::string m = Sub(Base(), "node uid=S1 type=sound parent=A", "node uid=S1 type=sound parent=N");
    m += "node uid=N type=random parent=R name=NewRand\n";
    const SyncDiff    d = DiffLibrary(Base(), m, Base());
    const SyncChange* mv = Has(d, "F|S1|node|parent");
    CHECK(mv && mv->side == SyncSide::Here && mv->node == "N" && mv->row == "N");
    CHECK(mv && mv->text == "Moved FS_C_01 from Walk_Concrete to NewRand");
    CHECK(mv && std::find(mv->needs.begin(), mv->needs.end(), "X|N") != mv->needs.end());
    SyncResult r;
    CHECK(ApplySync(Base(), m, Base(), d, PickSubject(d, "S1", SyncDir::Up), &r));
    CHECK(In(r.file, "node uid=N ") && In(r.file, "node uid=S1 type=sound parent=N"));
    CHECK(In(r.base, "uid=S1") && r.open.empty());
    CHECK(DiffLibrary(r.base, r.mine, r.file).changes.empty());
    // the same through the new container's gesture
    SyncResult r2;
    CHECK(ApplySync(Base(), m, Base(), d, PickRow(d, "N", SyncDir::Up), &r2));
    CHECK(In(r2.file, "node uid=S1 type=sound parent=N") && In(r2.file, "node uid=N "));
    // dropped (Down): the Sound goes back to Walk_Concrete, the new Random goes away, nothing is lost
    SyncResult r3;
    CHECK(ApplySync(Base(), m, Base(), d, PickRow(d, "N", SyncDir::Down), &r3));
    CHECK(In(r3.mine, "node uid=S1 type=sound parent=A") && !In(r3.mine, "node uid=N "));
    // probe P2: the file made Random N2 and moved FS_C_01 into it; Get file's on Walk_Concrete's parent row (N2 has no
    // row here: its changes sit under the Switch) and on the Sound's panel
    std::string f = Sub(Base(), "node uid=S1 type=sound parent=A", "node uid=S1 type=sound parent=N2");
    f += "node uid=N2 type=random parent=R name=FileRand\n";
    const SyncDiff    df = DiffLibrary(Base(), Base(), f);
    const SyncChange* fm = Has(df, "F|S1|node|parent");
    CHECK(fm && fm->side == SyncSide::File && fm->node == "N2" && fm->row == "R");
    CHECK(ContainerChangeKeys(df, "A").empty());
    SyncResult r4;
    CHECK(ApplySync(Base(), Base(), f, df, PickRow(df, "R", SyncDir::Down), &r4));
    CHECK(In(r4.mine, "node uid=S1 type=sound parent=N2") && In(r4.mine, "node uid=N2 "));
    SyncResult r5;
    CHECK(ApplySync(Base(), Base(), f, df, PickSubject(df, "S1", SyncDir::Down), &r5));
    CHECK(In(r5.mine, "node uid=S1 type=sound parent=N2") && In(r5.mine, "node uid=N2 "));
    CHECK(DiffLibrary(r5.base, r5.mine, r5.file).changes.empty());
}

static void TestMoveNeverDropsANode()
{
    // a move picked alone (Both: nothing pulled) into a container the other side lacks: the node keeps its old parent,
    // the move stays open, the base does not take it
    std::string m = Sub(Base(), "node uid=S1 type=sound parent=A", "node uid=S1 type=sound parent=N");
    m += "node uid=N type=random parent=R name=NewRand\n";
    const std::string f = Sub(Base(), "node uid=S1 type=sound parent=A", "node uid=S1 type=sound parent=B");
    const SyncDiff    d = DiffLibrary(Base(), m, f);
    CHECK(Has(d, "F|S1|node|parent") && Has(d, "F|S1|node|parent")->side == SyncSide::Both);
    SyncResult r;
    CHECK(ApplySync(Base(), m, f, d, {{"F|S1|node|parent", SyncDir::Up}}, &r));
    CHECK(In(r.file, "node uid=S1 type=sound parent=B"));  // still in the file, under its file parent
    CHECK(r.open.size() == 1 && r.sent == 0);
    CHECK(In(r.base, "node uid=S1 type=sound parent=A"));
    CHECK(Has(DiffLibrary(r.base, r.mine, r.file), "F|S1|node|parent"));
    // L3: here A moved under B, the file B under A: taking the file's move makes a loop: refused, both stay
    const std::string ml = Sub(Base(), "node uid=A type=random parent=R", "node uid=A type=random parent=B");
    const std::string fl = Sub(Base(), "node uid=B type=random parent=R", "node uid=B type=random parent=A");
    const SyncDiff    dl = DiffLibrary(Base(), ml, fl);
    SyncResult        rl;
    CHECK(ApplySync(Base(), ml, fl, dl, {{"F|B|node|parent", SyncDir::Down}}, &rl));
    CHECK(In(rl.mine, "node uid=A type=random parent=B") && In(rl.mine, "node uid=B type=random parent=R"));
    CHECK(rl.open.size() == 1);
}

static void TestAddedUnderRemoved()
{
    // M1 / probe P6: here a Sound added under Walk_Concrete, the file removed Walk_Concrete: ONE both change
    const std::string m = Base() + "node uid=S8 type=sound parent=A name=New\nsound kind=file\n";
    std::string       f = Base();
    f.erase(f.find("node uid=A "), f.find("node uid=B ") - f.find("node uid=A "));
    f = Sub(f, "case value=V1 child=A\n", "");
    const SyncDiff d = DiffLibrary(Base(), m, f);
    CHECK(!Has(d, "X|S8"));
    CHECK(Has(d, "X|A") && Has(d, "X|A")->side == SyncSide::Both && Has(d, "X|A")->here_text == "1 change");
    // Send mine of it: the file gets Walk_Concrete back with the new Sound
    SyncResult r;
    CHECK(ApplySync(Base(), m, f, d, {{"X|A", SyncDir::Up}}, &r));
    CHECK(In(r.file, "node uid=A ") && In(r.file, "node uid=S8 "));
    CHECK(r.sent == 1);
    // a Sound whose parent is gone on the target side is never counted as sent
    const SyncDiff d2 = DiffLibrary(Base(), Base() + "node uid=S9 type=sound parent=Q name=Orphan\nsound kind=file\n", Base());
    SyncResult     r2;
    CHECK(ApplySync(Base(), Base() + "node uid=S9 type=sound parent=Q name=Orphan\nsound kind=file\n", Base(), d2, {{"X|S9", SyncDir::Up}}, &r2));
    CHECK(r2.sent == 0 && r2.open.size() == 1 && !In(r2.base, "uid=S9"));
}

static void TestBothAddedFold()
{
    // M2 / probe P3: both sides added the same Sound; after an unrelated Apply the base has it, so removing it here later
    // reads as a here removal (not a file addition)
    const std::string add = "node uid=S9 type=sound parent=B name=FS_W_09\nsound kind=file\n";
    const std::string m = Sub(Base(), "vol=-4", "vol=-3") + add, f = Base() + add;
    const SyncDiff    d = DiffLibrary(Base(), m, f);
    CHECK(std::find(d.folds.begin(), d.folds.end(), "X|S9") != d.folds.end());
    SyncResult r;
    CHECK(ApplySync(Base(), m, f, d, PickRow(d, "A", SyncDir::Up), &r));
    CHECK(In(r.base, "node uid=S9 "));
    std::string m2 = r.mine;
    m2 = m2.substr(0, m2.find("node uid=S9"));
    const SyncDiff d2 = DiffLibrary(r.base, r.file_base, m2, r.file);
    CHECK(Has(d2, "X|S9") && Has(d2, "X|S9")->side == SyncSide::Here && Has(d2, "X|S9")->text == "Sound FS_W_09 removed");
    // added on both sides with different settings: both, and not folded until they agree
    const std::string m3 = Base() + "node uid=S9 type=sound parent=B name=FS_W_09 vol=-2\nsound kind=file\n";
    const SyncDiff    d3 = DiffLibrary(Base(), m3, f);
    CHECK(Has(d3, "F|S9|node|vol") && Has(d3, "F|S9|node|vol")->side == SyncSide::Both);
    CHECK(std::find(d3.folds.begin(), d3.folds.end(), "X|S9") == d3.folds.end());
}

static void TestValueNameMerge()
{
    // M4 / probe p2: the project's group has its own Mud (V7); another project added Mud (V3) to the file: matched by name
    std::vector<SwitchGroup> proj;
    SwitchGroup              g;
    g.uid = "G1";
    g.name = "Surface";
    g.values = {SwitchValue{"V1", "Concrete", {}, {}}, SwitchValue{"V2", "Wood", {}, {}}, SwitchValue{"V7", "mud", {}, {}}};
    proj.push_back(g);
    std::string f = Sub(Base(), "value uid=V2 name=Wood\n", "value uid=V2 name=Wood\nvalue uid=V3 name=Mud\n");
    f = Sub(f, "case value=V2 child=B\n", "case value=V2 child=B\ncase value=V3 child=B\n");
    const auto extra = NameMergeMap(proj, f, {});
    CHECK(extra.size() == 1 && extra[0].first == "V3" && extra[0].second == "V7");
    const std::string fp = TranslateUidsText(f, extra, true);
    const std::string m = Sub(Base(), "value uid=V2 name=Wood\n", "value uid=V2 name=Wood\nvalue uid=V7 name=mud\n");
    const SyncDiff    d = DiffLibrary(Base(), m, fp);
    CHECK(!Has(d, "VX|G1|V3") && !Has(d, "VX|G1|V7"));
    CHECK(Has(d, "C|R|V7") && Has(d, "C|R|V7")->side == SyncSide::File);
    SyncResult r;
    CHECK(ApplySync(Base(), m, fp, d, All(d, SyncDir::Down), &r));
    CHECK(In(r.mine, "case value=V7 child=B") && !In(r.mine, "V3"));
    // a group of another uid but the same name merges too; a value a file uid already maps to is never matched twice
    std::string f2 = Sub(Base(), "group uid=G1 name=Surface", "group uid=G9 name=surface");
    const auto  e2 = NameMergeMap(proj, f2, {});
    CHECK(e2.size() == 1 && e2[0].first == "G9" && e2[0].second == "G1");
    CHECK(NameMergeMap(proj, f, {{"V9", "V7"}}).empty());
    // probe p2: Send mine of a value only the file has (no case uses it) drops it from the file, for good
    const std::string fl = Sub(Base(), "value uid=V2 name=Wood\n", "value uid=V2 name=Wood\nvalue uid=V3 name=Mud\n");
    const SyncDiff    dl = DiffLibrary(Base(), Base(), fl);
    CHECK(Has(dl, "VX|G1|V3") && Has(dl, "VX|G1|V3")->row == "R");  // H5 / probe P9: on the root's row
    SyncResult rl;
    CHECK(ApplySync(Base(), Base(), fl, dl, {{"VX|G1|V3", SyncDir::Up}}, &rl));
    CHECK(rl.sent == 1 && !In(rl.file, "V3"));
    CHECK(DiffLibrary(rl.base, rl.file_base, rl.mine, rl.file).changes.empty());
    // used by a case in the file: it stays there, and is not listed again
    SyncResult ru;
    CHECK(ApplySync(Base(), Base(), f, DiffLibrary(Base(), Base(), f), {{"VX|G1|V3", SyncDir::Up}, {"C|R|V3", SyncDir::Up}}, &ru));
    CHECK(DiffLibrary(ru.base, ru.file_base, ru.mine, ru.file).changes.empty());
    // the store: a value added to a project group keeps only the roles no value there holds; a group this library did
    // not use keeps its name and default
    SoundStore st = MkFileStore();
    st.groups[0].values[0].roles = {"heel"};
    st.groups[0].default_value = "V2";
    SoundStore merged = MkFileStore();
    merged.groups[0].values.push_back(SwitchValue{"V3", "Mud", {}, {"heel", "toe"}});
    merged.groups[0].name = "FileName";
    merged.groups[0].default_value = "V1";
    SoundStore before = MkFileStore();
    before.groups.clear();
    CHECK(ReplaceLibraryFromMerge(st, "L1", merged, &before));
    CHECK(st.groups[0].name == "Surface" && st.groups[0].default_value == "V2");
    CHECK(st.groups[0].values.size() == 3 && st.groups[0].values[2].roles == std::vector<std::string>{"toe"});
    // a library name another library holds stays unique
    SoundStore two = MkFileStore();
    SoundLibrary other;
    other.uid = "L2";
    other.name = "Footsteps";
    other.root = "Z";
    two.libraries.push_back(other);
    SoundStore renamed = MkFileStore();
    renamed.libraries[0].name = "Footsteps";
    CHECK(ReplaceLibraryFromMerge(two, "L1", renamed));
    CHECK(two.libraries[0].name != "Footsteps");
}

static void TestRowsAndCounts()
{
    // H5 / probe P10: the file adds a Random (and its Sound) under Walk_Wood, which has no change of its own
    const std::string f = Base() + "node uid=M type=random parent=B name=Sub\nnode uid=SM type=sound parent=M name=x\nsound kind=file\n";
    const SyncDiff    d = DiffLibrary(Base(), Base(), f);
    CHECK(CountsOf(d, "B").file == 2 && ContainerChangeKeys(d, "B").size() == 2);
    CHECK(SyncTagText(CountsOf(d, "B")) == "in file \xC2\xB7 2");
    // the root's own: a library field and a container added in the file directly under it
    std::string f2 = Sub(Base(), "name=Foot", "name=Foot color=ff0000");
    f2 += "node uid=Q type=random parent=R name=Top\n";
    const SyncDiff d2 = DiffLibrary(Base(), Base(), f2);
    CHECK(ContainerChangeKeys(d2, "R").size() == 2 && CountsOf(d2, "R").file == 2);
    CHECK(TotalCounts(d2).file == 2);
}

static void TestCloseFrames()
{
    // L1 / probe P7: a second variation 0.8 ms after another one in the same text is its own variation
    const std::string m = Sub(Base(), "frame start=1.5 len=0.3\n", "frame start=1.5 len=0.3\nframe start=1.5008 len=0.1\n");
    const SyncDiff    d = DiffLibrary(Base(), m, Base());
    CHECK(d.changes.size() == 1 && d.changes[0].side == SyncSide::Here && d.changes[0].text == "Variation 3 of FS_C_01 added");
    SyncResult r;
    CHECK(ApplySync(Base(), m, Base(), d, All(d, SyncDir::Up), &r));
    CHECK(In(r.file, "frame start=1.5 len=0.3") && In(r.file, "frame start=1.5008 len=0.1"));
    CHECK(DiffLibrary(r.base, r.mine, r.file).changes.empty());
}

static void TestMutantsKilled()
{
    // Sanitize of the project copy: the file's removal of Walk_Wood taken alone (Both: nothing pulled) drops the case
    // that played it
    std::string f = Base();
    f.erase(f.find("node uid=B "));
    f = Sub(f, "case value=V2 child=B\n", "");
    const std::string m = Sub(Base(), "name=FS_W_01", "name=FS_W_01 vol=-1");
    const SyncDiff    d = DiffLibrary(Base(), m, f);
    CHECK(Has(d, "X|B") && Has(d, "X|B")->side == SyncSide::Both);
    SyncResult r;
    CHECK(ApplySync(Base(), m, f, d, {{"X|B", SyncDir::Down}}, &r));
    CHECK(!In(r.mine, "child=B") && !In(r.mine, "node uid=B "));
    // InsertPos: a container added between two siblings lands between them in the written file
    std::string mi = Base();
    mi = Sub(mi, "node uid=B type=random", "node uid=K type=random parent=R name=Mid\nnode uid=B type=random");
    const SyncDiff di = DiffLibrary(Base(), mi, Base());
    SyncResult     ri;
    CHECK(ApplySync(Base(), mi, Base(), di, All(di, SyncDir::Up), &ri));
    CHECK(ri.file.find("node uid=S1 ") < ri.file.find("node uid=K ") && ri.file.find("node uid=K ") < ri.file.find("node uid=B "));
    // the per-part gains' keys follow the uid map
    const std::string t = "RAVLIB 2\nframe start=0 pdb.V1=-3\n";
    CHECK(TranslateUidsText(t, {{"V1", "V9"}}, true) == "RAVLIB 2\nframe start=0 pdb.V9=-3\n");
    // a value the file has had since the last sync but the project lacks is never a change (decision 5)
    const std::string fv = Sub(Base(), "value uid=V2 name=Wood\n", "value uid=V2 name=Wood\nvalue uid=V3 name=Mud\n");
    CHECK(DiffLibrary(Base(), fv, Base(), fv).changes.empty());
    CHECK(DiffLibrary(Base(), Base(), Base(), fv).file == 1);
}

static SyncJournal Jr(const std::string& before, const std::string& after, uint64_t sb, uint64_t sa)
{
    SyncJournal j;
    j.lib_uid = "L1";
    j.file = "Foot.ravlib";
    j.before = before;
    j.after = after;
    j.base_before = sb;
    j.base_after = sa;
    return j;
}

static void TestJournalChain()
{
    const std::vector<SyncJournal> js = {Jr("f0", "f1", 10, 11), Jr("f1", "f2", 11, 12)};
    const std::vector<bool>        none = {false, false};
    // H2: the state is back (a revert, a reopened project) but REAPER's redo is not the sync: nothing
    CHECK(!JournalChainCandidate(js, none, 11, false, false));
    CHECK(DecideJournalChain(js, none, 11, false, false, true, SyncTextHash("f2")).step == JournalStep::None);
    // one real Ctrl+Z
    JournalPlan p1 = DecideJournalChain(js, none, 11, true, false, true, SyncTextHash("f2"));
    CHECK(p1.step == JournalStep::RestoreBefore && p1.write == "f1" && p1.undone == std::vector<bool>({false, true}));
    // M7: two quick Ctrl+Z (the timer saw only the second state): the file goes straight back to f0
    JournalPlan p2 = DecideJournalChain(js, none, 10, true, false, true, SyncTextHash("f2"));
    CHECK(p2.step == JournalStep::RestoreBefore && p2.write == "f0" && p2.undone == std::vector<bool>({true, true}));
    // written by someone else since: only the project is back, the journals are dropped
    JournalPlan p3 = DecideJournalChain(js, none, 10, true, false, true, SyncTextHash("other"));
    CHECK(p3.step == JournalStep::FileWrittenSince && p3.drop);
    // redo of both after the double undo
    JournalPlan p4 = DecideJournalChain(js, {true, true}, 12, false, true, true, SyncTextHash("f0"));
    CHECK(p4.step == JournalStep::RestoreAfter && p4.write == "f2" && p4.undone == none);
    // redo while the file moved: nothing written
    CHECK(DecideJournalChain(js, {true, true}, 12, false, true, true, SyncTextHash("x")).step == JournalStep::None);
    // a gap between the two Applies (someone wrote between them): no chained restore over it
    const std::vector<SyncJournal> gap = {Jr("f0", "f1", 10, 11), Jr("g1", "f2", 11, 12)};
    CHECK(DecideJournalChain(gap, none, 10, true, false, true, SyncTextHash("f2")).step == JournalStep::FileWrittenSince);
    // link state: every Apply moves it, even with the same texts
    LinkBase b;
    b.text = "t";
    LinkBase b2 = b;
    b2.syncs = 1;
    CHECK(LinkStateHash(b) != LinkStateHash(b2));
    SyncResult r;
    r.base = "B";
    r.file_base = "FB";
    const LinkBase nb = NextLinkBase(&b, "L1", r, {{"a", "b"}}, 77, "P.rpp", 70);
    CHECK(nb.text == "B" && nb.file_base == "FB" && nb.syncs == 1 && nb.synced_at == 77 && nb.uid_map.size() == 1);
    SyncResult partial;  // no common version and a partial Apply: the old ones stay, the sync still counts
    const LinkBase nb2 = NextLinkBase(&nb, "L1", partial, {}, 78, "P.rpp", 70);
    CHECK(nb2.text == "B" && nb2.file_base == "FB" && nb2.syncs == 2);
    CHECK(SyncAppliedToast(1, 0, "F.ravlib", 1) == "Applied: 1 written to F.ravlib (the version before is kept as F.ravlib.bak) \xC2\xB7 1 could not be applied: it stays listed");
}

static void TestFileBaseStored()
{
    // the file base round-trips through the project chunk, a line longer than 32 KB included; syncs too
    SoundStore s = MkFileStore();
    s.libraries[0].ravlib_path = "Foot.ravlib";
    LinkBase b;
    b.lib_uid = "L1";
    b.text = "RAVLIB 2\nlibrary uid=L1 name=Foot root=R\n";
    b.file_base = "RAVLIB 2 by=X at=1\nlibrary uid=L1 name=" + std::string(70000, 'a') + " root=R\n";
    b.syncs = 3;
    s.link_bases.push_back(b);
    SoundStore back;
    CHECK(ParseSoundStore(SerializeSoundStore(s), &back));
    CHECK(back.link_bases.size() == 1 && back.link_bases[0].text == b.text && back.link_bases[0].file_base == b.file_base);
    CHECK(back.link_bases[0].syncs == 3);
    // a link with no common version yet (empty text) still lets the first-sight pass adopt the file
    SoundStore e = MkFileStore();
    e.libraries[0].ravlib_path = "Foot.ravlib";
    LinkBase empty;
    empty.lib_uid = "L1";
    empty.syncs = 2;
    e.link_bases.push_back(empty);
    const std::string ft = SerializeRavlib(RavlibOf(e, "L1", nullptr), "Q", 5);
    CHECK(AdoptFileAsBase(e, "L1", ft, 9));
    CHECK(e.link_bases.size() == 1 && !e.link_bases[0].text.empty() && e.link_bases[0].file_base == ft && e.link_bases[0].syncs == 2);
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
    TestMoveIntoNewContainer();
    TestMoveNeverDropsANode();
    TestAddedUnderRemoved();
    TestBothAddedFold();
    TestValueNameMerge();
    TestRowsAndCounts();
    TestCloseFrames();
    TestMutantsKilled();
    TestJournalChain();
    TestFileBaseStored();
    if (g_fail == 0) std::printf("library_sync_test: all passed\n");
    return g_fail == 0 ? 0 : 1;
}
