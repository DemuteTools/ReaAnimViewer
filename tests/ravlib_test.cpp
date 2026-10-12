// SPDX-License-Identifier: MIT
//
// Host tests of src/ravlib.h (story 13-11): every row of the spec's I/O matrix and RavlibFileName.

#include "ravlib.h"

#include <cstdio>
#include <map>
#include <vector>
#include <string>

#include "media_db.h"
#include "sound_record.h"
#include "sound_switch.h"

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) {                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);             \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static SoundNode Node(const std::string& uid, const std::string& parent, SoundNodeType t, const std::string& name)
{
    SoundNode n;
    n.uid = uid;
    n.parent = parent;
    n.type = t;
    n.name = name;
    return n;
}

static SoundNode FileSound(const std::string& uid, const std::string& parent, const std::string& file)
{
    SoundNode n = Node(uid, parent, SoundNodeType::Sound, file);
    n.sound.source.kind = SoundSourceKind::File;
    n.sound.source.asset.name = file;
    n.sound.source.asset.size = 1000;
    n.sound.source.asset.length = 1.5;
    n.sound.source.asset.last_path = "C:/old/" + file;
    return n;
}

// Groups: foot_Material (Wood, Stone) used; Speed (Slow) used; Unused (X) not used.
static SoundStore RichStore()
{
    SoundStore s;
    SwitchGroup g;
    g.uid = "g000000000000001";
    g.name = "foot_Material";
    g.values = {{"v000000000000001", "Wood", {}}, {"v000000000000002", "Metal", {}}};
    s.groups.push_back(g);
    SwitchGroup u;
    u.uid = "g000000000000002";
    u.name = "Unused";
    u.values = {{"v000000000000003", "X", {}}};
    s.groups.push_back(u);
    SwitchGroup sp;
    sp.uid = "g000000000000003";
    sp.name = "Speed";
    sp.values = {{"v000000000000004", "Slow", {}}};
    s.groups.push_back(sp);

    SoundLibrary lib;
    lib.uid = "l000000000000001";
    lib.name = "Steps";
    lib.color = 0x1FF8800;
    lib.root = "n000000000000001";
    lib.user = true;
    lib.ravlib_path = "Steps.ravlib";
    SoundNode sw = Node("n000000000000001", "", SoundNodeType::Switch, "Steps");
    sw.group_uid = g.uid;
    sw.default_value = "v000000000000002";
    sw.preview_value = "v000000000000001";
    sw.cases = {{"v000000000000001", "n000000000000002", {}}, {"v000000000000002", "n000000000000003", {}}};
    sw.unassigned = {"n000000000000007"};
    sw.lands_in = "Foley";
    lib.nodes.push_back(sw);
    SoundNode r = Node("n000000000000002", sw.uid, SoundNodeType::Random, "Wood steps");
    r.avoid_repeat = 3;
    r.volume_db = -2.5;
    lib.nodes.push_back(r);
    SoundNode b = Node("n000000000000003", sw.uid, SoundNodeType::Blend, "Metal layers");
    lib.nodes.push_back(b);
    SoundNode f1 = FileSound("n000000000000004", r.uid, "wood1.wav");
    f1.sound.source.item_guid = "{AAAA}";
    f1.sound.mode = DetectMode::Silence;
    f1.sound.silence.threshold_db = -30.0;
    f1.sound.frames = {{0.0, 0.5, true, 0.0, false, {}}, {0.6, 0.4, false, -3.0, true, {}}};
    f1.sound.edited = true;
    f1.sound.fade_out = 0.008;               // Antho 2026-10-12: the Sound's default fade
    f1.sound.frames[1].fade_in = 0.003;      // and a variation's own
    f1.sound.frames[1].fade_out_shape = 2;
    lib.nodes.push_back(f1);
    SoundNode f2 = FileSound("n000000000000005", b.uid, "metal1.wav");
    f2.offset_ms = 0.0;
    lib.nodes.push_back(f2);
    SoundNode f3 = FileSound("n000000000000006", b.uid, "metal2.wav");
    f3.offset_ms = 25.0;
    lib.nodes.push_back(f3);
    SoundNode it = Node("n000000000000007", sw.uid, SoundNodeType::Sound, "Item sound");
    it.sound.source.kind = SoundSourceKind::Item;
    it.sound.source.item_guid = "{BBBB}";
    lib.nodes.push_back(it);
    s.libraries.push_back(lib);
    return s;
}

static void TestRoundTrip()
{
    const SoundStore s = RichStore();
    const SoundStore one = RavlibOf(s, "l000000000000001", nullptr);
    const std::string text = SerializeRavlib(one);
    CHECK(text.rfind("RAVLIB 1\n", 0) == 0);
    SoundStore back;
    CHECK(ParseRavlib(text, &back));
    CHECK(SerializeRavlib(back) == text);
    CHECK(back.libraries.size() == 1);
    CHECK(back.libraries[0].nodes.size() == s.libraries[0].nodes.size());
    CHECK(back.libraries[0].nodes[0].lands_in == "Foley");
    CHECK(back.libraries[0].nodes[3].sound.frames.size() == 2);
    // Fades travel in the file (optional fields: a v1 reader keeps them as unknown text).
    CHECK(back.libraries[0].nodes[3].sound.fade_out == 0.008 && back.libraries[0].nodes[3].sound.fade_in == kDefaultFadeIn);
    CHECK(back.libraries[0].nodes[3].sound.frames[1].fade_in == 0.003 &&
          back.libraries[0].nodes[3].sound.frames[1].fade_out_shape == 2 &&
          back.libraries[0].nodes[3].sound.frames[0].fade_in < 0.0);
    // A .ravlib without fades (written before them) reads with the defaults.
    SoundStore plain;
    CHECK(ParseRavlib("RAVLIB 1\nlibrary uid=l000000000000009 name=P root=n000000000000009\n"
                      "node uid=n000000000000009 type=sound name=S\nsound kind=file aname=a.wav\nframe start=0 len=1\n",
                      &plain));
    CHECK(plain.libraries.size() == 1 && plain.libraries[0].nodes[0].sound.fade_in == kDefaultFadeIn &&
          plain.libraries[0].nodes[0].sound.fade_out == kDefaultFadeOut &&
          plain.libraries[0].nodes[0].sound.frames[0].fade_out < 0.0);
    // A full library (with ravlib_path) round-trips too.
    SoundStore full;
    full.groups = one.groups;
    full.libraries = s.libraries;
    const std::string t2 = SerializeRavlib(full);
    SoundStore b2;
    CHECK(ParseRavlib(t2, &b2) && SerializeRavlib(b2) == t2);
    // A higher version reads best-effort.
    SoundStore b3;
    std::string t3 = text;
    t3.replace(0, 8, "RAVLIB 3");
    CHECK(ParseRavlib(t3, &b3) && b3.libraries.size() == 1);
}

static void TestNotRavlib()
{
    SoundStore out;
    out.groups.resize(1);
    const SoundStore s = RichStore();
    std::string sounds = SerializeSoundStore(s);
    CHECK(!ParseRavlib(sounds, &out));
    CHECK(!ParseRavlib("", &out));
    CHECK(!ParseRavlib("RAVLIB 0\n", &out));
    CHECK(!ParseRavlib("RAVLIBX 1\n", &out));
    SoundStore two = s;
    two.libraries.push_back(s.libraries[0]);
    two.libraries[1].uid = "l000000000000002";
    CHECK(!ParseRavlib(SerializeRavlib(two), &out));
    CHECK(!ParseRavlib("RAVLIB 1\n", &out));  // no library
    CHECK(out.groups.size() == 1 && out.libraries.empty());
    // Saved by a text editor: BOM, CRLF, blank lines before the magic, a tab after it.
    const std::string text = SerializeRavlib(RavlibOf(s, "l000000000000001", nullptr));
    std::string crlf;
    for (char c : text.substr(9)) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    SoundStore ed;
    CHECK(ParseRavlib("\xEF\xBB\xBF\r\n\nRAVLIB\t1\r\n" + crlf, &ed));
    CHECK(ed.libraries.size() == 1 && ed.libraries[0].uid == "l000000000000001");
}

static void TestExportForm()
{
    int items = -1;
    const SoundStore one = RavlibOf(RichStore(), "l000000000000001", &items);
    CHECK(items == 1);
    CHECK(one.groups.size() == 1 && one.groups[0].name == "foot_Material");
    const std::string text = SerializeRavlib(one);
    CHECK(text.find("ravlib=") == std::string::npos);
    CHECK(text.find("{AAAA}") == std::string::npos);
    CHECK(text.find("{BBBB}") != std::string::npos);  // an Item source keeps its guid (counted)
    CHECK(RavlibOf(RichStore(), "nope", &items).libraries.empty() && items == 0);
}

static void TestImportMerge()
{
    SoundStore file = RavlibOf(RichStore(), "l000000000000001", nullptr);
    file.groups[0].default_value = "v000000000000002";  // 13-18: the file's group default (Metal)
    SoundStore proj;
    SwitchGroup pg;
    pg.uid = "p000000000000001";
    pg.name = "Foot Material";
    pg.values = {{"p000000000000002", "wood", {}}};
    pg.default_value = "p000000000000002";
    proj.groups.push_back(pg);
    std::string uid, why;
    CHECK(AddRavlibLibrary(proj, file, "Steps.ravlib", &uid, &why));
    CHECK(proj.groups.size() == 1);
    CHECK(proj.groups[0].default_value == "p000000000000002");  // a merged group keeps the project's
    CHECK(proj.groups[0].values.size() == 2 && proj.groups[0].values[1].name == "Metal");
    const SoundLibrary* lib = FindLibrary(proj, uid);
    CHECK(lib && lib->user && lib->ravlib_path == "Steps.ravlib");
    if (lib) {
        const SoundNode* sw = FindNode(*lib, lib->root);
        CHECK(sw && sw->group_uid == "p000000000000001");
        CHECK(sw && sw->cases.size() == 2 && sw->cases[0].value_uid == "p000000000000002");
        CHECK(sw && sw->cases[1].value_uid == proj.groups[0].values[1].uid);
        CHECK(sw && sw->default_value == proj.groups[0].values[1].uid);
        CHECK(sw && sw->preview_value == "p000000000000002");
        CHECK(sw && FindNode(*lib, sw->cases[0].child_uid) != nullptr);
        CHECK(sw && sw->unassigned.size() == 1 && FindNode(*lib, sw->unassigned[0]) != nullptr);
        for (const SoundNode& n : lib->nodes)
            if (n.sound.source.kind == SoundSourceKind::File) CHECK(n.sound.source.item_guid.empty());
    }
    // 13-18: a Switch that followed the file's group default (Metal) keeps playing it on the merged
    // group whose project default is wood.
    {
        SoundStore f2 = RavlibOf(RichStore(), "l000000000000001", nullptr);
        f2.groups[0].default_value = "v000000000000002";
        f2.libraries[0].nodes[0].default_value.clear();
        SoundStore p2;
        p2.groups.push_back(pg);
        std::string u2;
        CHECK(AddRavlibLibrary(p2, f2, "Steps.ravlib", &u2, nullptr));
        const SoundLibrary* l2 = FindLibrary(p2, u2);
        const SoundNode*    s2 = l2 ? FindNode(*l2, l2->root) : nullptr;
        CHECK(p2.groups.size() == 1 && p2.groups[0].default_value == "p000000000000002");
        CHECK(s2 && s2->type == SoundNodeType::Switch && DefaultValue(p2, *s2) == p2.groups[0].values[1].uid);
        CHECK(p2.groups[0].values[1].name == "Metal");
    }
}

// 13-15a: a library's kit (the body parts it serves) travels in the file and back into a project.
static void TestKitTravels()
{
    SoundStore rich = RichStore();
    rich.libraries[0].kit = {"feet", "hands"};
    rich.libraries[0].kit_offered = true;
    const SoundStore one = RavlibOf(rich, "l000000000000001", nullptr);
    CHECK(one.libraries[0].kit.size() == 2);
    const std::string text = SerializeRavlib(one);
    CHECK(text.find("\nkit parts=feet,hands\n") != std::string::npos);  // "offered" is the project's, not exported
    SoundStore back;
    CHECK(ParseRavlib(text, &back));
    CHECK((back.libraries[0].kit == std::vector<std::string>{"feet", "hands"}) && !back.libraries[0].kit_offered);
    CHECK(SerializeRavlib(back) == text);
    SoundStore proj;
    std::string uid, why;
    CHECK(AddRavlibLibrary(proj, back, "Steps.ravlib", &uid, &why));
    const SoundLibrary* lib = FindLibrary(proj, uid);
    CHECK(lib && lib->kit == back.libraries[0].kit);
    // A file without the line has no kit.
    SoundStore plain = RavlibOf(RichStore(), "l000000000000001", nullptr);
    CHECK(SerializeRavlib(plain).find("kit ") == std::string::npos);
}

static void TestUidClash()
{
    SoundStore file = RavlibOf(RichStore(), "l000000000000001", nullptr);
    file.groups[0].default_value = "v000000000000002";  // 13-18: travels in the file
    {
        SoundStore back;
        CHECK(ParseRavlib(SerializeRavlib(file), &back) && back.groups[0].default_value == "v000000000000002");
    }
    SoundStore proj;
    SwitchGroup other;
    other.uid = "q000000000000001";
    other.name = "Surface";
    other.values = {{"v000000000000002", "Grass", {}}};  // same uid as the file's Metal
    proj.groups.push_back(other);
    std::string uid;
    CHECK(AddRavlibLibrary(proj, file, "a.ravlib", &uid, nullptr));
    CHECK(proj.groups.size() == 2);
    const SwitchGroup& added = proj.groups[1];
    CHECK(added.name == "foot_Material");
    CHECK(added.values[1].name == "Metal" && added.values[1].uid != "v000000000000002");
    const SoundLibrary* lib = FindLibrary(proj, uid);
    const SoundNode* sw = lib ? FindNode(*lib, lib->root) : nullptr;
    CHECK(sw && sw->group_uid == added.uid);
    CHECK(sw && sw->cases[1].value_uid == added.values[1].uid);
    CHECK(sw && sw->default_value == added.values[1].uid);
    CHECK(added.default_value == added.values[1].uid);  // a new group's default remapped with its values
    CHECK(proj.groups[0].values[0].uid == "v000000000000002");
}

static void TestAlreadyThere()
{
    SoundStore proj = RichStore();
    const std::string before = SerializeSoundStore(proj);
    const SoundStore file = RavlibOf(proj, "l000000000000001", nullptr);
    std::string uid, why;
    CHECK(!AddRavlibLibrary(proj, file, "x.ravlib", &uid, &why));
    CHECK(why == "Already in this project" && uid == "l000000000000001");
    CHECK(SerializeSoundStore(proj) == before);
    SoundStore empty;
    CHECK(!AddRavlibLibrary(proj, empty, "x.ravlib", &uid, &why));
}

static void TestLandsInAndLayer()
{
    SoundStore s = RichStore();
    SoundLibrary& lib = s.libraries[0];
    // Sound under a Random with lands "Foley2", root lands "Foley".
    FindNode(lib, "n000000000000002")->lands_in = "Foley2";
    CHECK(EffectiveLandsIn(lib, "n000000000000004") == "Foley2");
    CHECK(EffectiveLandsIn(lib, "n000000000000005") == "Foley");  // inherited from root
    for (SoundNode& n : lib.nodes) n.lands_in.clear();
    CHECK(EffectiveLandsIn(lib, "n000000000000004") == kDefaultLandsIn);
    FindNode(lib, "n000000000000004")->lands_in = "Mine";
    CHECK(EffectiveLandsIn(lib, "n000000000000004") == "Mine");
    lib.user = false;
    CHECK(EffectiveLandsIn(lib, "n000000000000004") == kDefaultLandsIn);
    // 13-16: the nearest Blend and its child on the path (BlendLayerOf's cases, now uids).
    std::string blend, child;
    CHECK(BlendLayerPath(lib, "n000000000000006", &blend, &child) && blend == "n000000000000003" &&
          child == "n000000000000006");
    CHECK(BlendLayerPath(lib, "n000000000000005", &blend, &child) && child == "n000000000000005");
    CHECK(!BlendLayerPath(lib, "n000000000000004", &blend, &child) && blend.empty() && child.empty());
    // A Sound under a Random that is a Blend child: the Random is its layer.
    SoundNode r2 = Node("n000000000000008", "n000000000000003", SoundNodeType::Random, "Layer 2 random");
    lib.nodes.push_back(r2);
    lib.nodes.push_back(FileSound("n000000000000009", r2.uid, "metal3.wav"));
    CHECK(BlendLayerPath(lib, "n000000000000009", &blend, &child) && blend == "n000000000000003" &&
          child == "n000000000000008");
    CHECK(BlendLayerPath(lib, "n000000000000008", &blend, &child) && child == "n000000000000008");
}

static void TestResolve()
{
    AssetIdentity id;
    id.name = "a.wav";
    id.size = 100;
    id.last_path = "C:/x/a.wav";
    MediaDbIndex idx;
    DbFile f;
    f.path = "D:/db/a.wav";
    f.size = 100;
    idx.Add(f);
    auto exists = [](int64_t sz) { return [sz](const std::string&) { return sz; }; };
    CHECK(ResolveAssetPath(id, exists(100), &idx) == "C:/x/a.wav");
    CHECK(ResolveAssetPath(id, exists(99), &idx) == "D:/db/a.wav");
    CHECK(ResolveAssetPath(id, exists(-1), &idx) == "D:/db/a.wav");
    AssetIdentity any = id;
    any.size = -1;
    CHECK(ResolveAssetPath(any, exists(55), nullptr) == "C:/x/a.wav");
    DbFile f2 = f;
    f2.path = "E:/other/a.wav";
    idx.Add(f2);
    AssetIdentity amb = id;
    amb.last_path = "C:/zz/a.wav";
    CHECK(ResolveAssetPath(amb, exists(-1), &idx).empty());
    CHECK(ResolveAssetPath(amb, exists(-1), nullptr).empty());
}

static void TestApplyLocated()
{
    SoundStore s = RichStore();
    SoundLibrary second = s.libraries[0];
    second.uid = "l000000000000002";
    s.libraries.push_back(second);
    FindNode(s.libraries[0], "n000000000000006")->sound.source.asset.name = "WOOD1.wav";
    AssetIdentity who;
    who.name = "wood1.wav";
    who.size = 1000;
    const int n = ApplyLocated(s, {{who, "F:/new/wood1.wav"}});
    CHECK(n == 3);
    CHECK(FindNode(s.libraries[1], "n000000000000004")->sound.source.asset.last_path == "F:/new/wood1.wav");
    CHECK(FindNode(s.libraries[0], "n000000000000005")->sound.source.asset.last_path == "C:/old/metal1.wav");
    const std::string before = SerializeSoundStore(s);
    CHECK(ApplyLocated(s, {{who, "F:/new/wood1.wav"}}) == 0);
    AssetIdentity none;
    none.name = "nothing.wav";
    CHECK(ApplyLocated(s, {{none, "F:/n.wav"}}) == 0);
    CHECK(SerializeSoundStore(s) == before);
    // Unknown size filled from the located one.
    FindNode(s.libraries[0], "n000000000000005")->sound.source.asset.size = -1;
    AssetIdentity m;
    m.name = "metal1.wav";
    m.size = 777;
    CHECK(ApplyLocated(s, {{m, "F:/m.wav"}}) == 1);
    CHECK(FindNode(s.libraries[0], "n000000000000005")->sound.source.asset.size == 777);
    // Length: more than 1.5 ms apart is another file, within it the same.
    AssetIdentity a, b;
    a.name = b.name = "x.wav";
    a.size = b.size = 10;
    a.length = 1.0;
    b.length = 1.002;
    CHECK(!SameAssetIdentity(a, b));
    b.length = 1.001;
    CHECK(SameAssetIdentity(a, b));
    // The picked file of another size (re-rendered): the Sounds set to it take its size and length.
    SoundStore p = RichStore();
    AssetIdentity stored = FindNode(p.libraries[0], "n000000000000004")->sound.source.asset;
    AssetIdentity picked;
    picked.name = "wood1.wav";
    picked.size = 2000;
    picked.length = 2.0;
    picked.last_path = "F:/re/wood1.wav";
    CHECK(ApplyLocated(p, {{stored, picked.last_path}}, &picked) == 1);
    const AssetIdentity& now = FindNode(p.libraries[0], "n000000000000004")->sound.source.asset;
    CHECK(now.last_path == picked.last_path && now.size == 2000 && now.length == 2.0);
    auto sized = [](const std::string& path) { return path == "F:/re/wood1.wav" ? int64_t(2000) : int64_t(-1); };
    CHECK(ResolveAssetPath(now, sized, nullptr) == picked.last_path);
    CHECK(ApplyLocated(p, {{now, picked.last_path}}, &picked) == 0);
}

static void TestFolderChildren()
{
    std::vector<int> kids;
    int              last = -1;
    // 0 Outer(1) 1 Dest(1) 2 a(0) 3 b(-2) 4 after(0): Dest's last child also closes Outer.
    CHECK(FolderChildren({1, 1, 0, -2, 0}, 1, &kids, &last) == 1);
    CHECK(kids == std::vector<int>({2, 3}) && last == 3);
    // 0 Dest(1) 1 a(0) 2 b(-1): closes only Dest.
    CHECK(FolderChildren({1, 0, -1}, 0, &kids, &last) == 0);
    CHECK(kids == std::vector<int>({1, 2}) && last == 2);
    // 0 Dest(1) 1 Sub(1) 2 x(-2) 3 y(0): a sub-folder's last track closes Sub and Dest.
    CHECK(FolderChildren({1, 1, -2, 0}, 0, &kids, &last) == 0);
    CHECK(kids == std::vector<int>({1}) && last == 2);
    // 0 Outer(1) 1 Dest(1) 2 Sub(1) 3 x(-3): closes Sub, Dest and Outer.
    CHECK(FolderChildren({1, 1, 1, -3}, 1, &kids, &last) == 1);
    CHECK(kids == std::vector<int>({2}) && last == 3);
    // An empty folder at the bottom.
    CHECK(FolderChildren({0, 1}, 1, &kids, &last) == 0);
    CHECK(kids.empty() && last == 1);
}

static void TestFileName()
{
    CHECK(RavlibFileName("Steps", {}) == "Steps.ravlib");
    CHECK(RavlibFileName("a/b:c*?\"<>|", {}) == "a_b_c______.ravlib");
    CHECK(RavlibFileName("  . ", {}) == "Library.ravlib");
    CHECK(RavlibFileName(" Steps. ", {"steps.RAVLIB"}) == "Steps 2.ravlib");
    CHECK(RavlibFileName("Steps", {"Steps.ravlib", "STEPS 2.ravlib"}) == "Steps 3.ravlib");
}

int main()
{
    TestRoundTrip();
    TestNotRavlib();
    TestExportForm();
    TestImportMerge();
    TestKitTravels();
    TestUidClash();
    TestAlreadyThere();
    TestLandsInAndLayer();
    TestResolve();
    TestApplyLocated();
    TestFileName();
    TestFolderChildren();
    if (g_fail) {
        std::printf("ravlib_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("ravlib_test: all passed\n");
    return 0;
}
