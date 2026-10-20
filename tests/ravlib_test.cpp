// SPDX-License-Identifier: MIT
//
// Host tests of src/ravlib.h (story 13-11): every row of the spec's I/O matrix and RavlibFileName.

#include "ravlib.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <vector>
#include <string>

#include "library_tree_edit.h"
#include "media_db.h"
#include "sound_place_plan.h"
#include "sound_record.h"
#include "sound_switch.h"
#include "sound_zones.h"  // 13-62i: MigrateLegacyZones, SanitizeZones

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
    n.sound.source.item_db = 18.5;  // imported from an item at +18.5 dB (travels in the file)
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
    f1.sound.frames[1].fade_out = 0.004;
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

// 13-62i: every Sound laid out as zones, as a read lays it out (so a write and a read give the same text).
static SoundStore Laid(SoundStore s)
{
    for (SoundLibrary& l : s.libraries)
        for (SoundNode& n : l.nodes)
            if (n.type == SoundNodeType::Sound) {
                MigrateLegacyZones(n.sound);
                SanitizeZones(n.sound);
            }
    return s;
}

static void TestRoundTrip()
{
    const SoundStore s = Laid(RichStore());
    const SoundStore one = RavlibOf(s, "l000000000000001", nullptr);
    const std::string text = SerializeRavlib(one);
    CHECK(text.rfind("RAVLIB 2\n", 0) == 0);  // 13-23: kRavlibVersion
    SoundStore back;
    CHECK(ParseRavlib(text, &back));
    CHECK(SerializeRavlib(back) == text);
    CHECK(back.libraries.size() == 1);
    CHECK(back.libraries[0].nodes.size() == s.libraries[0].nodes.size());
    CHECK(back.libraries[0].nodes[0].lands_in == "Foley");
    CHECK(back.libraries[0].nodes[3].sound.frames.size() == 2);
    for (const SoundNode& x : back.libraries[0].nodes)
        if (x.type == SoundNodeType::Sound && x.sound.source.kind == SoundSourceKind::File) CHECK(x.sound.source.item_db == 18.5);
    // Fades travel in the file (optional fields: a v1 reader keeps them as unknown text).
    CHECK(back.libraries[0].nodes[3].sound.fade_out == 0.008 && back.libraries[0].nodes[3].sound.fade_in == kDefaultFadeIn);
    {  // 13-62i: a variation's own fades are its zone's
        const SoundData& fd = back.libraries[0].nodes[3].sound;
        CHECK(fd.frames[1].zones.size() == 1 && fd.frames[1].zones[0].fade_in == 0.003 && fd.frames[1].zones[0].fade_out_shape == 2 &&
              fd.frames[0].zones.size() == 1 && !fd.frames[0].zones[0].fade_in);
    }
    // A .ravlib without fades (written before them) reads with the defaults.
    SoundStore plain;
    CHECK(ParseRavlib("RAVLIB 1\nlibrary uid=l000000000000009 name=P root=n000000000000009\n"
                      "node uid=n000000000000009 type=sound name=S\nsound kind=file aname=a.wav\nframe start=0 len=1\n",
                      &plain));
    CHECK(plain.libraries.size() == 1 && plain.libraries[0].nodes[0].sound.fade_in == kDefaultFadeIn &&
          plain.libraries[0].nodes[0].sound.fade_out == kDefaultFadeOut &&
          plain.libraries[0].nodes[0].sound.frames[0].fade_out < 0.0 &&
          plain.libraries[0].nodes[0].sound.frames[0].zones.size() == 1);  // 13-62i: read as one zone
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
    CHECK(lib && LibraryLinked(*lib) && lib->ravlib_path == "Steps.ravlib");
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
    SoundStore rich = Laid(RichStore());
    rich.libraries[0].kit = {"legs", "arms"};
    rich.libraries[0].kit_offered = true;
    const SoundStore one = RavlibOf(rich, "l000000000000001", nullptr);
    CHECK(one.libraries[0].kit.size() == 2);
    const std::string text = SerializeRavlib(one);
    CHECK(text.find("\nkit parts=legs,arms\n") != std::string::npos);  // "offered" is the project's, not exported
    SoundStore back;
    CHECK(ParseRavlib(text, &back));
    CHECK((back.libraries[0].kit == std::vector<std::string>{"legs", "arms"}) && !back.libraries[0].kit_offered);
    CHECK(SerializeRavlib(back) == text);
    // Story 10-9-1: an old .ravlib's kit (feet / hands) reads in the new words.
    {
        std::string old = text;
        old.replace(old.find("kit parts=legs,arms"), 19, "kit parts=feet,body");
        SoundStore o;
        CHECK(ParseRavlib(old, &o) && (o.libraries[0].kit == std::vector<std::string>{"legs", "body"}));
        CHECK(SerializeRavlib(o).find("\nkit parts=legs,body\n") != std::string::npos);
    }
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
    lib.ravlib_path.clear();  // 13-77a: Lands in works on every library, linked or not
    CHECK(EffectiveLandsIn(lib, "n000000000000004") == "Mine");
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
    auto only = [](const std::string& on_disk) {
        return [on_disk](const std::string& p) { return p == on_disk ? int64_t(100) : int64_t(-1); };
    };
    CHECK(ResolveAssetPath(id, only("D:/db/a.wav"), &idx) == "D:/db/a.wav");
    // The database still lists it but the file is gone (moved, other PC's drive): missing.
    CHECK(ResolveAssetPath(id, exists(-1), &idx).empty());
    CHECK(ResolveAssetPath(id, nullptr, &idx) == "D:/db/a.wav");
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

    // 13-50 review: renamed (wood1.wav is now wood1_v2.wav): the Sound and those sharing its old file
    // take the new identity whole; another Sound of the same name elsewhere is left alone.
    SoundStore q = RichStore();
    SoundLibrary q2 = q.libraries[0];
    q2.uid = "l000000000000002";
    q.libraries.push_back(q2);
    const AssetIdentity old_id = FindNode(q.libraries[0], "n000000000000004")->sound.source.asset;
    FindNode(q.libraries[1], "n000000000000006")->sound.source.asset.name = "wood1.wav";
    FindNode(q.libraries[1], "n000000000000006")->sound.source.asset.last_path = "Z:/elsewhere/wood1.wav";
    AssetIdentity renamed;
    renamed.name = "wood1_v2.wav";
    renamed.size = 3000;
    renamed.length = -1.0;
    renamed.last_path = "F:/re/wood1_v2.wav";
    const int r = ApplyRelinked(q, q.libraries[0].uid, "n000000000000004", old_id, renamed);
    const AssetIdentity& self = FindNode(q.libraries[0], "n000000000000004")->sound.source.asset;
    CHECK(self.name == "wood1_v2.wav" && self.last_path == renamed.last_path && self.size == 3000 && self.length < 0.0);
    CHECK(FindNode(q.libraries[1], "n000000000000004")->sound.source.asset.name == "wood1_v2.wav");
    CHECK(FindNode(q.libraries[1], "n000000000000006")->sound.source.asset.last_path == "Z:/elsewhere/wood1.wav");
    CHECK(r == 2);
    CHECK(ApplyRelinked(q, q.libraries[0].uid, "n000000000000004", old_id, renamed) == 0);
    CHECK(ApplyRelinked(q, "nolib", "nonode", AssetIdentity{}, renamed) == 0);

    // Spec missing-sounds-window: an item Sound whose item is gone remembers its file; Locate relinks that file
    // (its kind kept) only when the caller says the item is gone.
    SoundStore it = RichStore();
    SoundNode* is = FindNode(it.libraries[0], "n000000000000004");
    is->sound.source.kind = SoundSourceKind::Item;
    is->sound.source.item_guid = "{GONE}";
    const AssetIdentity was = is->sound.source.asset;
    CHECK(ApplyLocated(it, {{was, "F:/found/wood1.wav"}}) == 0);  // not by default
    CHECK(is->sound.source.asset.last_path == was.last_path);
    auto gone = [](const SoundNode& x) { return x.sound.source.item_guid == "{GONE}"; };
    CHECK(ApplyLocated(it, {{was, "F:/found/wood1.wav"}}, nullptr, gone) == 1);
    CHECK(is->sound.source.asset.last_path == "F:/found/wood1.wav" && is->sound.source.kind == SoundSourceKind::Item &&
          is->sound.source.item_guid == "{GONE}");
    CHECK(ApplyRelinked(it, it.libraries[0].uid, "n000000000000004", was, renamed) == 0);
    CHECK(ApplyRelinked(it, it.libraries[0].uid, "n000000000000004", was, renamed, gone) == 1);
    CHECK(is->sound.source.asset.name == "wood1_v2.wav" && is->sound.source.kind == SoundSourceKind::Item);
    // Found by name: an older item Sound with no remembered file takes it; one that remembers a file is left alone.
    is->sound.source.asset = AssetIdentity{};
    FoundByName fb;
    fb.lib_uid = it.libraries[0].uid;
    fb.node_uid = "n000000000000004";
    fb.found.last_path = "E:\\Boom\\Metal\\Handle 02.wav";
    fb.found.size = 42;
    CHECK(ApplyFoundByName(it, {fb}) == 1);
    CHECK(is->sound.source.asset.name == "Handle 02.wav" && is->sound.source.asset.size == 42 &&
          is->sound.source.kind == SoundSourceKind::Item);
    CHECK(ApplyFoundByName(it, {fb}) == 0);
    fb.node_uid = "nonode";
    CHECK(ApplyFoundByName(it, {fb}) == 0);
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

// ---- 13-24: Convert a Project Library to a User Library ----

static SoundNode ItemSound(const std::string& uid, const std::string& parent, const std::string& guid, DetectMode mode)
{
    SoundNode n = Node(uid, parent, SoundNodeType::Sound, "snd " + uid);
    n.sound.source.kind = SoundSourceKind::Item;
    n.sound.source.item_guid = guid;
    n.sound.mode = mode;
    return n;
}

static ItemFileFacts OneTake(const std::string& type, const std::string& path, double len = 1.2, int64_t size = 4800)
{
    ItemFileFacts f;
    f.found = true;
    f.takes.push_back({type, path, len, size});
    return f;
}

static std::string Why(const SoundData& d, const ItemFileFacts& f)
{
    SoundSource s;
    s.item_guid = "untouched";
    std::string why;
    CHECK(!FileSourceFromItem(d, f, &s, &why));
    CHECK(s.item_guid == "untouched");
    return why;
}

static void TestFileSourceFromItem()
{
    // WAV item, Silence, frames with dB / fades: the file's identity, item_guid kept.
    SoundNode n = ItemSound("s1", "", "{AAA}", DetectMode::Silence);
    SoundSource s;
    std::string why;
    CHECK(FileSourceFromItem(n.sound, OneTake("WAVE", "C:/a/kick.wav"), &s, &why));
    CHECK(s.kind == SoundSourceKind::File && s.item_guid == "{AAA}");
    CHECK(s.asset.name == "kick.wav" && s.asset.last_path == "C:/a/kick.wav");
    CHECK(s.asset.size == 4800 && s.asset.length > 1.19 && s.asset.length < 1.21);
    {  // the item's gain is kept with the file: the live item's when read, else the stored one
        ItemFileFacts lf = OneTake("WAVE", "C:/a/kick.wav");
        lf.entry_ok = true;
        lf.entry.item_db = 18.5;
        SoundSource ls;
        CHECK(FileSourceFromItem(n.sound, lf, &ls, &why) && ls.item_db == 18.5);
        SoundData stored = n.sound;
        stored.source.item_db = 6.0;
        CHECK(FileSourceFromItem(stored, OneTake("WAVE", "C:/a/kick.wav"), &ls, &why) && ls.item_db == 6.0);
    }
    // Unknown size / length stay unknown; a backslash path gives its name.
    CHECK(FileSourceFromItem(n.sound, OneTake("FLAC", "C:\\a\\b\\x.flac", 0.0, -1), &s, &why));
    CHECK(s.asset.name == "x.flac" && s.asset.size == -1 && s.asset.length < 0.0);
    // Refusals.
    CHECK(Why(n.sound, OneTake("MIDI", "C:/a/x.mid")) == "MIDI");
    CHECK(Why(n.sound, OneTake("MIDIPOOL", "")) == "MIDI");
    CHECK(Why(n.sound, OneTake("RPP_PROJECT", "C:/a/sub.rpp")) == "a subproject");
    CHECK(Why(n.sound, OneTake("SECTION", "C:/a/kick.wav")) == "a reversed or section take");
    CHECK(Why(n.sound, OneTake("EMPTY", "")) == "no media file");
    CHECK(Why(n.sound, OneTake("WAVE", "")) == "no media file");
    ItemFileFacts none;
    CHECK(Why(n.sound, none) == "its item is not in the project");
    ItemFileFacts pile = OneTake("WAVE", "C:/a/kick.wav");
    pile.pile = true;
    CHECK(Why(n.sound, pile) == "an item pile");
    SoundNode noguid = ItemSound("s2", "", "", DetectMode::Whole);
    CHECK(Why(noguid.sound, OneTake("WAVE", "C:/a/kick.wav")) == "its item is not in the project");
    // Takes mode: one file for every take converts; different files do not.
    SoundNode tk = ItemSound("s3", "", "{BBB}", DetectMode::Takes);
    ItemFileFacts same = OneTake("WAVE", "C:/a/Take.wav");
    same.takes.push_back({"WAVE", "c:\\A\\take.WAV", 1.2, 4800});
    same.takes.push_back({"WAVE", "C:/a/Take.wav", 1.2, 4800});
    CHECK(FileSourceFromItem(tk.sound, same, &s, &why) && s.asset.name == "Take.wav");
    ItemFileFacts diff = OneTake("WAVE", "C:/a/a.wav");
    diff.takes.push_back({"WAVE", "C:/a/b.wav", 1.0, 100});
    CHECK(Why(tk.sound, diff) == "takes of different files");
    ItemFileFacts midi_take = OneTake("WAVE", "C:/a/a.wav");
    midi_take.takes.push_back({"MIDI", "", 1.0, -1});
    CHECK(Why(tk.sound, midi_take) == "MIDI");
    // Whole mode, two takes of different files: the active one.
    SoundNode wh = ItemSound("s4", "", "{CCC}", DetectMode::Whole);
    diff.active = 1;
    CHECK(FileSourceFromItem(wh.sound, diff, &s, &why) && s.asset.name == "b.wav" && s.asset.size == 100);
    // Relative path made absolute.
    CHECK(AbsoluteMediaPath("Audio/x.wav", "C:/p") == "C:/p/Audio/x.wav");
    CHECK(AbsoluteMediaPath("Audio/x.wav", "C:\\p\\") == "C:\\p\\Audio/x.wav");
    CHECK(AbsoluteMediaPath("D:/x.wav", "C:/p") == "D:/x.wav");
    CHECK(AbsoluteMediaPath("\\\\srv\\x.wav", "C:/p") == "\\\\srv\\x.wav");
    CHECK(AbsoluteMediaPath("x.wav", "") == "x.wav");
}

static void TestConvertLibrary()
{
    SoundStore s = RichStore();
    // A Project Library: root Random, 2 WAV Sounds (Silence with frames), 1 MIDI, 1 File already.
    SoundLibrary lib;
    lib.uid = "L000000000000009";
    lib.name = "Foley";
    lib.root = "n000000000000090";
    lib.nodes.push_back(Node(lib.root, "", SoundNodeType::Random, "Foley"));
    SoundNode a = ItemSound("n000000000000091", lib.root, "{G1}", DetectMode::Silence);
    SoundFrame f1;
    f1.start = 0.1;
    f1.length = 0.3;
    f1.db = -3.0;
    f1.fade_in = 0.01;
    f1.hand = true;
    SoundFrame f2;
    f2.start = 0.5;
    f2.length = 0.2;
    f2.on = false;
    a.sound.frames = {f1, f2};
    a.sound.edited = true;
    a.sound.silence.threshold_db = -30.0;
    a.sound.fade_out = 0.02;
    a.volume_db = -6.0;
    lib.nodes.push_back(a);
    lib.nodes.push_back(ItemSound("n000000000000092", lib.root, "{G2}", DetectMode::Whole));
    lib.nodes.push_back(ItemSound("n000000000000093", lib.root, "{G3}", DetectMode::Whole));
    lib.nodes.push_back(FileSound("n000000000000094", lib.root, "done.wav"));
    s.libraries.push_back(lib);
    auto facts = [](const std::string& guid) {
        if (guid == "{G1}") return OneTake("WAVE", "C:/a/one.wav");
        if (guid == "{G2}") return OneTake("WAVE", "C:/a/two.wav");
        return OneTake("MIDI", "");
    };
    ConvertReport r;
    CHECK(ConvertLibraryToUser(s, lib.uid, "Foley.ravlib", facts, &r));
    const SoundLibrary* out = FindLibrary(s, lib.uid);
    CHECK(out && LibraryLinked(*out) && !out->user && out->ravlib_path == "Foley.ravlib" && out->name == "Foley");
    CHECK(r.sounds == 4 && r.converted == 2 && r.project_only.size() == 1);
    CHECK(r.project_only.size() == 1 && r.project_only[0].uid == "n000000000000093" && r.project_only[0].why == "MIDI");
    if (out) {
        const SoundNode* na = FindNode(*out, "n000000000000091");
        CHECK(na && na->sound.source.kind == SoundSourceKind::File && na->sound.source.item_guid == "{G1}");
        CHECK(na && na->sound.source.asset.name == "one.wav");
        // Everything else kept.
        CHECK(na && na->sound.frames.size() == 2 && na->sound.frames[0].db == -3.0 && na->sound.frames[0].hand &&
              na->sound.frames[0].fade_in == 0.01 && !na->sound.frames[1].on && na->sound.edited &&
              na->sound.silence.threshold_db == -30.0 && na->sound.fade_out == 0.02 && na->volume_db == -6.0 &&
              na->sound.mode == DetectMode::Silence && na->name == a.name);
        const SoundNode* nm = FindNode(*out, "n000000000000093");
        CHECK(nm && nm->sound.source.kind == SoundSourceKind::Item && nm->sound.source.item_guid == "{G3}");
        const SoundNode* nf = FindNode(*out, "n000000000000094");
        CHECK(nf && nf->sound.source.kind == SoundSourceKind::File && nf->sound.source.asset.name == "done.wav");
        // The .ravlib form drops the converted Sounds' item GUIDs and counts the project-only one.
        int              items = -1;
        const SoundStore one = RavlibOf(s, lib.uid, &items);
        CHECK(items == 1);
        const SoundNode* xa = one.libraries.empty() ? nullptr : FindNode(one.libraries.front(), "n000000000000091");
        CHECK(xa && xa->sound.source.item_guid.empty());
    }
    const std::string txt = SaveToFileToastText("Foley", "Foley.ravlib", r);
    CHECK(txt == "Foley saved to Foley.ravlib in the User Library folder: other projects can Add it. 1 of 4 sounds are "
                 "project-only and will be missing in other projects (snd n000000000000093: MIDI)");
    ConvertReport clean;
    CHECK(SaveToFileToastText("X", "X.ravlib", clean) == "X saved to X.ravlib in the User Library folder: other projects can Add it");
    ConvertReport many;
    many.sounds = 5;
    for (int i = 0; i < 5; ++i) many.project_only.push_back({"u", "S" + std::to_string(i), "MIDI"});
    CHECK(SaveToFileToastText("M", "M.ravlib", many).find("(S0: MIDI, S1: MIDI, S2: MIDI, +2 more)") != std::string::npos);
    // Already linked / unknown uid: false, store unchanged.
    const std::string before = SerializeSoundStore(s);
    CHECK(!ConvertLibraryToUser(s, lib.uid, "Other.ravlib", facts, &r));
    CHECK(!ConvertLibraryToUser(s, "nope", "Other.ravlib", facts, &r));
    CHECK(SerializeSoundStore(s) == before);
}

// 13-24 review: frames frozen from the item (in source time) when a Sound becomes a File Sound.
static ItemFileFacts WithEntry(ItemFileFacts f, double source_len, double start_offs, double item_len,
                               double playrate = 1.0)
{
    f.entry_ok = true;
    f.entry.source_len = source_len;
    f.entry.start_offs = start_offs;
    f.entry.item_len = item_len;
    f.entry.playrate = playrate;
    return f;
}

static bool Near(double a, double b) { return a > b - 1e-9 && a < b + 1e-9; }

static void TestConvertFreezesFrames()
{
    SoundLibrary lib;
    lib.uid = "L000000000000010";
    lib.name = "Freeze";
    lib.root = "n000000000000100";
    lib.nodes.push_back(Node(lib.root, "", SoundNodeType::Random, "Freeze"));
    // Never detected (several items added at once): the item's trimmed portion, not the whole file.
    lib.nodes.push_back(ItemSound("n000000000000101", lib.root, "{T1}", DetectMode::Silence));
    // Never detected, take markers: TakeMarkers (no peaks, never the file's silence).
    lib.nodes.push_back(ItemSound("n000000000000102", lib.root, "{T2}", DetectMode::Silence));
    // Never detected, 2 takes of one file: Takes.
    lib.nodes.push_back(ItemSound("n000000000000103", lib.root, "{T3}", DetectMode::Silence));
    // Detected Whole at playrate 2 (item length 0.8 s = 1.6 s of the file), not edited: 1.6 s.
    SoundNode w = ItemSound("n000000000000104", lib.root, "{T4}", DetectMode::Whole);
    SoundFrame wf;
    wf.start = 12.0;
    wf.length = 0.8;
    wf.db = -2.0;
    w.sound.frames = {wf};
    lib.nodes.push_back(w);
    // The same, edited by hand: kept.
    SoundNode we = w;
    we.uid = "n000000000000105";
    we.sound.source.item_guid = "{T5}";
    we.sound.edited = true;
    lib.nodes.push_back(we);
    // Takes at playrate 2, not edited: each take's length from the item.
    SoundNode tk = ItemSound("n000000000000106", lib.root, "{T6}", DetectMode::Takes);
    SoundFrame t1, t2;
    t1.start = 0.0;
    t1.length = 0.5;
    t2.start = 2.0;
    t2.length = 0.5;
    tk.sound.frames = {t1, t2};
    lib.nodes.push_back(tk);
    // Never detected, item facts unread: left empty.
    lib.nodes.push_back(ItemSound("n000000000000107", lib.root, "{T7}", DetectMode::Silence));
    SoundStore s;
    s.libraries.push_back(lib);
    auto facts = [](const std::string& guid) {
        const ItemFileFacts wav = OneTake("WAVE", "C:/a/long.wav", 60.0);
        if (guid == "{T1}") return WithEntry(wav, 60.0, 12.0, 0.8);
        if (guid == "{T2}") {
            ItemFileFacts f = WithEntry(wav, 60.0, 0.0, 60.0);
            f.entry.slices = {Slice{1.0, 2.0, "a"}, Slice{4.0, 1.5, "b"}};
            return f;
        }
        if (guid == "{T3}" || guid == "{T6}") {
            ItemFileFacts f = wav;
            f.takes.push_back(f.takes[0]);
            f = WithEntry(f, 60.0, 0.0, 0.5, guid == "{T6}" ? 2.0 : 1.0);
            SoundFrame a, b;
            a.start = 0.0;
            b.start = 2.0;
            f.entry.takes = {a, b};  // length 0: the item's length (in source time)
            return f;
        }
        if (guid == "{T4}" || guid == "{T5}") return WithEntry(wav, 60.0, 12.0, 0.8, 2.0);
        return wav;  // {T7}: no entry
    };
    ConvertReport r;
    CHECK(ConvertLibraryToUser(s, lib.uid, "Freeze.ravlib", facts, &r));
    CHECK(r.converted == 7 && r.project_only.empty());
    const SoundLibrary* out = FindLibrary(s, lib.uid);
    if (!out) return;
    const SoundNode* a = FindNode(*out, "n000000000000101");
    CHECK(a && a->sound.source.kind == SoundSourceKind::File && a->sound.mode == DetectMode::Whole &&
          a->sound.frames.size() == 1 && Near(a->sound.frames[0].start, 12.0) && Near(a->sound.frames[0].length, 0.8) &&
          !a->sound.edited);
    // In another project the placed slice is that portion, not the 60 s file.
    if (a) {
        const SliceApply sl = FileSliceFor(a->sound, 0);
        CHECK(sl.set && Near(sl.start_offs, 12.0) && Near(sl.length, 0.8));
    }
    const SoundNode* b = FindNode(*out, "n000000000000102");
    CHECK(b && b->sound.mode == DetectMode::TakeMarkers && b->sound.frames.size() == 2 &&
          Near(b->sound.frames[1].start, 4.0) && Near(b->sound.frames[1].length, 1.5));
    const SoundNode* c = FindNode(*out, "n000000000000103");
    CHECK(c && c->sound.mode == DetectMode::Takes && c->sound.frames.size() == 2 &&
          Near(c->sound.frames[1].start, 2.0) && Near(c->sound.frames[1].length, 0.5));
    const SoundNode* d = FindNode(*out, "n000000000000104");
    CHECK(d && d->sound.mode == DetectMode::Whole && d->sound.frames.size() == 1 &&
          Near(d->sound.frames[0].start, 12.0) && Near(d->sound.frames[0].length, 1.6) && d->sound.frames[0].zones.size() == 1 &&
          d->sound.frames[0].zones[0].db == -2.0);  // 13-62i: laid out as a zone, the gain in it
    const SoundNode* e = FindNode(*out, "n000000000000105");
    CHECK(e && e->sound.frames.size() == 1 && Near(e->sound.frames[0].length, 0.8));
    const SoundNode* t = FindNode(*out, "n000000000000106");
    CHECK(t && t->sound.frames.size() == 2 && Near(t->sound.frames[0].length, 1.0) && Near(t->sound.frames[1].length, 1.0));
    const SoundNode* g = FindNode(*out, "n000000000000107");
    CHECK(g && g->sound.source.kind == SoundSourceKind::File && g->sound.frames.empty() &&
          g->sound.mode == DetectMode::Silence);
}


// 13-23 (spec 13-20-23-25 D2 / D8): a divided Sound's group (with its roles) travels in the file; a
// new project gets the group and the same parts, zones and syncs; an existing group keeps the
// project's role map; a v1 file still reads (its Sounds combined).
static SoundStore DividedStore()
{
    SoundStore  s = RichStore();
    SwitchGroup c;
    c.uid = "g000000000000009";
    c.name = "Contact";
    c.values = {{"v000000000000091", "Back", {}, {"heel"}}, {"v000000000000092", "Front", {}, {"toe", "toe_end"}}};
    s.groups.push_back(c);
    SoundData& d = s.libraries[0].nodes[3].sound;  // wood1.wav, under a Random
    d.divide_group = c.uid;
    d.divide_values = {"v000000000000091", "v000000000000092"};
    d.role_map = {{"hand", "v000000000000092"}};
    d.lanes_split = true;
    d.sync_mode = SyncMode::Peak;
    d.sync_offset_ms = -5.0;
    d.cut_fade = 0.003;
    d.mono = MonoMode::Sum;  // 13-41
    d.zones = {{0.1, 0.05, "", {}}, {0.3, 0.02, "v000000000000091", {}}};
    d.frames[0].cuts = {0.2};
    d.frames[0].cuts_hand = true;
    d.frames[0].syncs = {0.01, 0.21};
    d.frames[0].hand_syncs = {0.21};
    d.frames[0].part_db = {{"v000000000000091", 2.5}, {"v000000000000092", -1.5}};  // 13-28
    s.libraries[0].nodes[5].layer_roles = {"toe", "toe_end"};  // metal2.wav, a Blend layer (13-31)
    s.libraries[0].nodes[5].layer_keep_offset = true;
    s.libraries[0].nodes[4].layer_roles = {"heel"};
    return s;
}

// 13-62 (matrix ".ravlib"): a divided Sound with owners and part settings, opened in another project:
// the same owners and settings, their value uids remapped to the project's group.
static void TestPartsTravel()
{
    SoundStore src = DividedStore();
    SoundData& d = src.libraries[0].nodes[3].sound;
    d.frames[0].owners = {"v000000000000091", "v000000000000092"};
    PartSettings front;
    front.value = "v000000000000092";
    front.gain_db = -3.0;
    front.tail_ms = 40.0;
    front.mono = MonoMode::Left;
    d.parts = {front};
    const std::string text = SerializeRavlib(RavlibOf(src, "l000000000000001", nullptr));
    CHECK(text.find("part v=v000000000000092 tail=40 gain=-3 mono=left\n") != std::string::npos);
    CHECK(text.find(" own=v000000000000091,v000000000000092") != std::string::npos);
    SoundStore back;
    // 13-62h: read back as zones (the old keys migrated once); written again, it reads the same.
    CHECK(ParseRavlib(text, &back) && !back.libraries[0].nodes[3].sound.frames[0].zones.empty());
    SoundStore again;
    CHECK(ParseRavlib(SerializeRavlib(back), &again) && SerializeRavlib(again) == SerializeRavlib(back));
    // Into a project whose Contact group has other uids: owners and settings follow the merged values.
    SoundStore  proj;
    SwitchGroup pc;
    pc.uid = "p000000000000009";
    pc.name = "contact";
    pc.values = {{"p000000000000091", "Back", {}, {"heel"}}, {"p000000000000092", "Front", {}, {"toe"}}};
    proj.groups.push_back(pc);
    std::string uid;
    CHECK(AddRavlibLibrary(proj, back, "Steps.ravlib", &uid, nullptr));
    const SoundLibrary* lib = FindLibrary(proj, uid);
    const SoundNode*    snd = nullptr;
    if (lib)
        for (const SoundNode& n : lib->nodes)
            if (n.name == "wood1.wav") snd = &n;
    CHECK(snd != nullptr);
    if (!snd) return;
    const SoundData& q = snd->sound;
    CHECK((q.divide_values == std::vector<std::string>{"p000000000000091", "p000000000000092"}));
    // 13-62h: each zone's part follows the merged values; Front's gain is in its zones.
    bool remapped = !q.frames[0].zones.empty(), front_db = false;
    for (const EditZone& z : q.frames[0].zones) {
        remapped = remapped && (z.part == "p000000000000091" || z.part == "p000000000000092");
        if (z.part == "p000000000000092") front_db = z.db == -1.5 - 3.0;  // its part_db + its own gain
    }
    CHECK(remapped && front_db);
    CHECK(q.parts.size() == 1 && q.parts[0].value == "p000000000000092" && PartGainDb(q, "p000000000000092") == 0.0 &&
          PartTailMs(q, "p000000000000092") == 40.0 && PartMono(q, "p000000000000092") == MonoMode::Left);
    std::string v;
    CHECK(PartAt(q, 0, 0.05, &v) && v == "p000000000000091");
}

static void TestDividedTravels()
{
    const SoundStore one = RavlibOf(DividedStore(), "l000000000000001", nullptr);
    CHECK(one.groups.size() == 2 && one.groups[1].name == "Contact");  // the divide group comes along
    const std::string text = SerializeRavlib(one);
    CHECK(text.rfind("RAVLIB 2\n", 0) == 0);
    CHECK(text.find("value uid=v000000000000092 name=Front roles=toe,toe_end\n") != std::string::npos);
    CHECK(text.find(" pdb.v000000000000091=2.5 pdb.v000000000000092=-1.5\n") != std::string::npos);
    CHECK(text.find(" roles=toe,toe_end koff=1") != std::string::npos);  // 13-31
    CHECK(text.find(" mono=sum") != std::string::npos);                  // 13-41
    SoundStore back;
    CHECK(ParseRavlib(text, &back));  // 13-62h: migrated into zones; a second round trip is stable
    SoundStore again;
    CHECK(ParseRavlib(SerializeRavlib(back), &again) && SerializeRavlib(again) == SerializeRavlib(back));

    // Into a new project: a fresh group with its roles, every uid of the Sound follows.
    SoundStore  proj;
    std::string uid;
    CHECK(AddRavlibLibrary(proj, back, "Steps.ravlib", &uid, nullptr));
    const SwitchGroup* g = nullptr;
    for (const SwitchGroup& x : proj.groups)
        if (x.name == "Contact") g = &x;
    CHECK(g && g->values.size() == 2 && g->values[0].roles == std::vector<std::string>{"heel"} &&
          g->values[1].roles == (std::vector<std::string>{"toe", "toe_end"}));
    const SoundLibrary* lib = FindLibrary(proj, uid);
    const SoundNode*    snd = nullptr;
    const SoundNode*    lay = nullptr;
    const SoundNode*    whole = nullptr;
    if (lib)
        for (const SoundNode& n : lib->nodes) {
            if (n.name == "wood1.wav") snd = &n;
            if (n.name == "metal2.wav") lay = &n;
            if (n.name == "metal1.wav") whole = &n;
        }
    CHECK(g && snd && lay && whole);
    if (g && snd && lay && whole) {
        const SoundData& d = snd->sound;
        CHECK(d.divide_group == g->uid);
        CHECK(d.divide_values == (std::vector<std::string>{g->values[0].uid, g->values[1].uid}));
        CHECK(d.role_map.size() == 1 && d.role_map[0].role == "hand" && d.role_map[0].value == g->values[1].uid);
        CHECK(d.zones.empty() && !d.frames[0].zones.empty());  // 13-62h: folded into the variations' zones
        CHECK(d.lanes_split && d.sync_mode == SyncMode::Peak && d.sync_offset_ms == -5.0 && d.cut_fade == 0.003 &&
              d.mono == MonoMode::Sum);
        // 13-62h: the cut is now a zone edge; the hand sync point the zone's own snap offset.
        CHECK(d.frames[0].cuts.empty() && d.frames[0].hand_syncs.empty());
        bool snap = false, edge = false;
        for (const EditZone& z : d.frames[0].zones) {
            snap = snap || (z.snap && std::fabs(*z.snap - 0.21) < 1e-9);
            edge = edge || std::fabs(z.a - 0.2) < 1e-9;
        }
        CHECK(snap && edge);
        CHECK(lay->layer_roles == (std::vector<std::string>{"toe", "toe_end"}) && lay->layer_keep_offset);
        CHECK(whole->layer_roles == std::vector<std::string>{"heel"} && !whole->layer_keep_offset);
        // 13-28: the per-part gains follow their values; a frame without writes none.
        // 13-62h: the per-part gains are the zones' own gains.
        bool gains = !d.frames[0].zones.empty();
        for (const EditZone& z : d.frames[0].zones)
            gains = gains && (z.part == g->values[0].uid ? z.db == 2.5 : z.db == -1.5);
        CHECK(d.frames[0].part_db.empty() && gains);
        // The same parts: Back owns the start (the excluded zone a hole in it) and the zone given to it
        // inside Front.
        const std::vector<PartSpan> back_spans = PlacedSpans(d, 0, g->values[0].uid);
        CHECK(back_spans.size() == 3 && back_spans[0].start == 0.0 && std::fabs(back_spans[0].length - 0.1) < 1e-9 &&
              std::fabs(back_spans[1].start - 0.15) < 1e-9 && std::fabs(back_spans[2].start - 0.3) < 1e-9);
    }

    // Into a project that has Contact already (other roles): its map stands; a value it adds brings
    // only the keys no value of the group holds.
    SoundStore  p2;
    SwitchGroup pc;
    pc.uid = "p000000000000009";
    pc.name = "contact";
    pc.values = {{"p000000000000091", "Back", {}, {"heel", "toe"}}};
    p2.groups.push_back(pc);
    std::string u2;
    CHECK(AddRavlibLibrary(p2, back, "Steps.ravlib", &u2, nullptr));
    const SwitchGroup* m = FindGroup(p2, "p000000000000009");
    CHECK(m && m->values.size() == 2 && m->values[0].roles == (std::vector<std::string>{"heel", "toe"}));
    CHECK(m && m->values[1].name == "Front" && m->values[1].roles == std::vector<std::string>{"toe_end"});
    const SoundLibrary* l2 = FindLibrary(p2, u2);
    const SoundNode*    s2 = nullptr;
    if (l2)
        for (const SoundNode& n : l2->nodes)
            if (n.name == "wood1.wav") s2 = &n;
    CHECK(m && s2 && s2->sound.divide_group == m->uid && s2->sound.divide_values[0] == "p000000000000091" &&
          s2->sound.divide_values[1] == m->values[1].uid);
    bool parts_follow = s2 && m && !s2->sound.frames[0].zones.empty();  // 13-62h: each zone's part follows
    if (s2 && m)
        for (const EditZone& z : s2->sound.frames[0].zones)
            parts_follow = parts_follow && (z.part == "p000000000000091" || z.part == m->values[1].uid);
    CHECK(parts_follow);

    // A v1 file (written before parts) still reads: combined, no syncs.
    SoundStore v1;
    CHECK(ParseRavlib("RAVLIB 1\nlibrary uid=l000000000000009 name=P root=n000000000000009\n"
                      "node uid=n000000000000009 type=sound name=S\nsound kind=file aname=a.wav\nframe start=0 len=1\n",
                      &v1));
    CHECK(v1.libraries.size() == 1 && v1.libraries[0].nodes[0].sound.divide_group.empty() &&
          v1.libraries[0].nodes[0].sound.frames[0].syncs.empty() &&
          v1.libraries[0].nodes[0].sound.sync_mode == SyncMode::Attack &&
          v1.libraries[0].nodes[0].sound.sync_offset_ms == kDefaultSyncOffsetMs);
}

// ---- 13-77a: one kind of library, linked to a file or not ---------------------------------------------

static const char kOldUser[] =
    "RAVSOUNDS 1\nlibrary uid=L1 name=Foot root=n1 user=1 ravlib=Foot.ravlib\nnode uid=n1 type=random name=Foot\n"
    "node uid=n2 type=sound parent=n1 name=Step vol=-3\n"
    "library uid=L2 name=NeverSaved root=m1 user=1\nnode uid=m1 type=random\n"
    "library uid=L3 name=Proj root=p1\nnode uid=p1 type=random\n";

static void TestOldProjectConversion()
{
    SoundStore s;
    CHECK(ParseSoundStore(kOldUser, &s) && s.libraries.size() == 3);
    // A User Library with a file is linked, a never-saved one and a Project Library are not; no base yet.
    CHECK(LibraryLinked(s.libraries[0]) && s.libraries[0].ravlib_path == "Foot.ravlib");
    CHECK(!LibraryLinked(s.libraries[1]) && !LibraryLinked(s.libraries[2]));
    CHECK(s.link_bases.empty());
    // `user` is read and never written again; the link is the ravlib name.
    const std::string out = SerializeSoundStore(s);
    CHECK(out.find("user=1") == std::string::npos && out.find("ravlib=Foot.ravlib") != std::string::npos);
    SoundStore again;
    CHECK(ParseSoundStore(out, &again) && LibraryLinked(again.libraries[0]) && !LibraryLinked(again.libraries[1]));
    // Lands in reads on every library (the Project one too).
    FindNode(s.libraries[2], "p1")->lands_in = "Foley";
    CHECK(EffectiveLandsIn(s.libraries[2], "p1") == "Foley");

    // File equal: the project's copy was autosaved to the file (an old file even holds `user=1`, and was written
    // by another project): the common version is set silently, from the file's header.
    const std::string mine = SerializeRavlib(RavlibOf(s, "L1", nullptr));
    std::string       file_text = SerializeRavlib(RavlibOf(s, "L1", nullptr), "Other project.rpp", 1700000000);
    const size_t      lp = file_text.find("library uid=L1");
    CHECK(lp != std::string::npos);
    file_text.insert(file_text.find('\n', lp), " user=1");
    CHECK(SameRavlibContent(mine, file_text));
    SoundStore a = s;
    CHECK(AdoptFileAsBase(a, "L1", file_text, 1700000100));
    const LinkBase* b = FindLinkBase(a, "L1");
    CHECK(b && b->text == mine && b->file_by == "Other project.rpp" && b->file_at == 1700000000 && b->synced_at == 1700000100);
    CHECK(!AdoptFileAsBase(a, "L1", file_text, 5));  // it has one now
    // The text round-trips through the project chunk.
    SoundStore rt;
    CHECK(ParseSoundStore(SerializeSoundStore(a), &rt) && rt.link_bases.size() == 1 && rt.link_bases[0].text == mine &&
          rt.link_bases[0].file_by == "Other project.rpp");

    // File different: it stays without a common version.
    SoundStore other = s;
    FindNode(other.libraries[0], "n2")->volume_db = -9.0;
    const std::string other_text = SerializeRavlib(RavlibOf(other, "L1", nullptr), "Else.rpp", 5);
    CHECK(!SameRavlibContent(mine, other_text));
    SoundStore c = s;
    CHECK(!AdoptFileAsBase(c, "L1", other_text, 9) && c.link_bases.empty());
    // Not linked: nothing to adopt.
    CHECK(!AdoptFileAsBase(c, "L2", mine, 9) && !AdoptFileAsBase(c, "L3", mine, 9));
}

// Review 13-77a L4 (mutants F, G): an unlinked library, or an Attached one with a stale ravlib_path, adopts nothing
// even when the file's text is exactly its own; an Attached library is not linked.
static void TestAdoptNeedsLink()
{
    SoundStore s = RichStore();
    s.libraries[0].ravlib_path.clear();
    const std::string own = SerializeRavlib(RavlibOf(s, s.libraries[0].uid, nullptr), "P.rpp", 1);
    CHECK(!AdoptFileAsBase(s, s.libraries[0].uid, own, 9) && s.link_bases.empty());
    s.libraries[0].ravlib_path = "Steps.ravlib";
    s.libraries[0].attached = true;
    CHECK(!LibraryLinked(s.libraries[0]));
    CHECK(!AdoptFileAsBase(s, s.libraries[0].uid, own, 9) && s.link_bases.empty());
    s.libraries[0].attached = false;
    CHECK(LibraryLinked(s.libraries[0]) && AdoptFileAsBase(s, s.libraries[0].uid, own, 9));
}

// Review 13-77a L4 (mutant A): the uid map holds the groups and values the merge put under the project's own uids,
// not only the renewed ones: 13-77b writes the merged groups back under the file's uids with it.
static void TestAddMapHoldsMerged()
{
    SoundStore file = RavlibOf(RichStore(), "l000000000000001", nullptr);
    SoundStore proj;
    SwitchGroup pg;
    pg.uid = "p000000000000001";
    pg.name = "Foot Material";
    pg.values = {{"p000000000000002", "wood", {}}};
    proj.groups.push_back(pg);
    std::string                                      uid, why;
    std::vector<std::pair<std::string, std::string>> map;
    LinkBase                                         base;
    CHECK(AddRavlibLibrary(proj, file, "Steps.ravlib", &uid, &why, &map, &base));
    auto has = [&](const std::string& a, const std::string& b) {
        for (const auto& kv : map)
            if (kv.first == a && kv.second == b) return true;
        return false;
    };
    CHECK(has("g000000000000001", "p000000000000001"));  // the merged group
    CHECK(has("v000000000000001", "p000000000000002"));  // Wood merged into the project's wood
    CHECK(proj.groups[0].values.size() == 2 && proj.groups[0].values[1].name == "Metal");  // Metal: a new value
    CHECK(base.uid_map == map);
}

// Review 13-77a H1: the file work of every gesture is decided by pure functions.
static void TestFileDecisions()
{
    SoundLibrary lib;
    lib.uid = "L1";
    lib.name = "Foot";
    const std::vector<FolderRavlib> none = {{"Other.ravlib", "L2"}, {"Bad.ravlib", ""}};
    // Save to a file: free -> write; the folder holds this uid -> link to it, never write; linked / Attached -> refuse.
    CHECK(DecideSaveToFile(lib, none).action == SaveFileAction::WriteNew);
    const std::vector<FolderRavlib> held = {{"Other.ravlib", "L2"}, {"Foot shared.ravlib", "L1"}};
    SaveFileDecision                d = DecideSaveToFile(lib, held);
    CHECK(d.action == SaveFileAction::LinkExisting && d.file == "Foot shared.ravlib");
    CHECK(LinkedExistingToastText(d.file) ==
          "Foot shared.ravlib already holds this library: linked to it, nothing written. Sync with file will show the differences.");
    SoundLibrary linked = lib;
    linked.ravlib_path = "X.ravlib";
    CHECK(DecideSaveToFile(linked, none).action == SaveFileAction::Refuse);
    SoundLibrary att = lib;
    att.attached = true;
    CHECK(DecideSaveToFile(att, none).action == SaveFileAction::Refuse);
    // The link without a common version: nothing else changes.
    SoundStore s;
    s.libraries.push_back(lib);
    LinkBase b;
    b.lib_uid = "L1";
    s.link_bases.push_back(b);  // a stale base is dropped
    CHECK(LinkLibraryWithoutBase(s, "L1", "Foot shared.ravlib") && s.libraries[0].ravlib_path == "Foot shared.ravlib" &&
          s.link_bases.empty());
    CHECK(!LinkLibraryWithoutBase(s, "L1", "Again.ravlib"));  // already linked

    // Save it again.
    SoundLibrary gone = lib;
    gone.ravlib_path = "Foot.ravlib";
    CHECK(DecideSaveAgain(gone, {{"Foot.ravlib", "L1"}}, true).action == RewriteAction::Nothing);   // Ok again (stale click)
    CHECK(DecideSaveAgain(gone, none, false).action == RewriteAction::WriteSame);                   // really gone
    CHECK(DecideSaveAgain(gone, none, true).action == RewriteAction::WriteNew);                     // the name exists on disk
    RewriteDecision r = DecideSaveAgain(gone, {{"Renamed.ravlib", "L1"}}, false);                    // renamed in Explorer
    CHECK(r.action == RewriteAction::LinkExisting && r.file == "Renamed.ravlib");
    CHECK(DecideSaveAgain(gone, {{"foot.ravlib", "L2"}}, true).action == RewriteAction::WriteNew);  // another library's
    CHECK(DecideSaveAgain(gone, {{"Foot.ravlib", ""}}, true).action == RewriteAction::WriteNew);     // unreadable
    CHECK(DecideSaveAgain(lib, none, false).action == RewriteAction::Nothing);                       // unlinked
    // Windows folds accented capitals: "\xC3\x89t\xC3\xA9.ravlib" and "\xC3\xA9t\xC3\xA9.ravlib" are one file.
    SoundLibrary acc = lib;
    acc.ravlib_path = "\xC3\x89t\xC3\xA9.ravlib";
    CHECK(SameFileName("\xC3\x89t\xC3\xA9.ravlib", "\xC3\xA9t\xC3\xA9.RAVLIB"));
    CHECK(!SameFileName("Ete.ravlib", "\xC3\xA9te.ravlib") && SameFileName("A\xC3\x80.ravlib", "a\xC3\xA0.ravlib"));
    CHECK(SameFileName("\xCE\x91.ravlib", "\xCE\xB1.ravlib") && SameFileName("\xD0\x96.ravlib", "\xD0\xB6.ravlib"));
    CHECK(!SameFileName("\xC3\x97.ravlib", "\xC3\xB7.ravlib"));  // the multiplication sign has no lower case
    CHECK(LinkedFileStateOf(acc, {{"\xC3\xA9t\xC3\xA9.ravlib", "L2"}}) == LinkedFileState::OtherLibrary);
    CHECK(DecideSaveAgain(acc, {{"\xC3\xA9t\xC3\xA9.ravlib", "L2"}}, true).action == RewriteAction::WriteNew);
    CHECK(RavlibFileName("\xC3\x89t\xC3\xA9", {"\xC3\xA9t\xC3\xA9.ravlib"}) == "\xC3\x89t\xC3\xA9 2.ravlib");
    // The DLL installs the Win32 fold; the decisions follow it.
    SetFileNameFold([](const std::string& s) { return std::string(s.size(), 'x'); });
    CHECK(SameFileName("a", "b"));
    SetFileNameFold(nullptr);
    CHECK(!SameFileName("a", "b"));

    // L2: each bad state has its own words.
    CHECK(LinkedFileWordsOf(LinkedFileState::Missing, "F.ravlib").title == "F.ravlib is not in the User Library folder");
    CHECK(LinkedFileWordsOf(LinkedFileState::OtherLibrary, "F.ravlib").title == "F.ravlib holds another library");
    CHECK(LinkedFileWordsOf(LinkedFileState::Unreadable, "F.ravlib").title == "F.ravlib cannot be read");
    CHECK(LinkedFileWordsOf(LinkedFileState::Unreadable, "F.ravlib").row_tip.find("cannot be read") != std::string::npos);
    CHECK(LinkedFileWordsOf(LinkedFileState::OtherLibrary, "F.ravlib").row_tip.find("not found") == std::string::npos);
    CHECK(LinkedFileWordsOf(LinkedFileState::Ok, "F.ravlib").title.empty());

    // Locate.
    RelinkDecision l = DecideRelink(none, "L1", true, "Picked.ravlib");
    CHECK(l.action == RelinkAction::LinkPicked && l.file == "Picked.ravlib");
    CHECK(DecideRelink(none, "L1", false, "Out.ravlib").action == RelinkAction::CopyIn);
    l = DecideRelink(held, "L1", false, "Out.ravlib");  // the folder holds this uid: no second copy
    CHECK(l.action == RelinkAction::LinkExisting && l.file == "Foot shared.ravlib");

    // Import.
    SoundStore proj;
    proj.libraries.push_back(lib);
    CHECK(DecideImport(proj, none, "L1").action == ImportAction::AlreadyInProject);
    CHECK(DecideImport(proj, none, "L9").action == ImportAction::Copy);
    ImportDecision im = DecideImport(proj, held, "L2");
    CHECK(im.action == ImportAction::Refuse && im.file == "Other.ravlib");
    CHECK(ImportRefusedToastText(im.file) == "Other.ravlib in the User Library folder is that library: Add it from the list");
    CHECK(DecideImport(proj, held, "L1").action == ImportAction::AlreadyInProject);  // the project wins over the folder
}

// Decision 2: first sight picks the linked libraries without a common version whose file is in the folder, once each.
static void TestFirstSightTargets()
{
    SoundStore s;
    for (const char* u : {"L1", "L2", "L3", "L4"}) {
        SoundLibrary l;
        l.uid = u;
        l.ravlib_path = std::string(u) + ".ravlib";
        s.libraries.push_back(l);
    }
    s.libraries[3].ravlib_path.clear();  // unlinked
    LinkBase b;
    b.lib_uid = "L2";
    b.text = "RAVLIB 2\nlibrary uid=L2 root=r\n";
    s.link_bases.push_back(b);  // has a base already
    LinkBase e;
    e.lib_uid = "L1";
    s.link_bases.push_back(e);  // review 13-77b: a link with an empty text (an Apply with no common version) has none
    const std::vector<FolderRavlibText> files = {{"l1.RAVLIB", "L1", "t1"}, {"L2.ravlib", "L2", "t2"},
                                                 {"L3.ravlib", "L9", "t3"}, {"L4.ravlib", "L4", "t4"}};
    std::vector<std::string> asked;
    auto                     once = [&](const std::string& u) {
        asked.push_back(u);
        return true;
    };
    const auto t = FirstSightTargets(s, files, once);
    CHECK(t.size() == 1 && t[0].first == "L1" && t[0].second == "t1");  // L2 has a base, L3's file holds another uid, L4 unlinked
    CHECK((asked == std::vector<std::string>{"L1", "L3"}));
    CHECK(FirstSightTargets(s, files, [](const std::string&) { return false; }).empty());  // seen already
}

static void TestLinkUnlink()
{
    SoundStore s;
    CHECK(ParseSoundStore(kOldUser, &s));
    const std::string before = SerializeSoundStore(s);
    // Save to a file: linked to the name, the common version = what was written, the writer in its header.
    const LinkBase base = MakeLinkBase(s, "L3", "Proj.rpp", 1700000000, 1700000001);
    CHECK(LinkLibraryToFile(s, "L3", "Proj.ravlib", base));
    CHECK(LibraryLinked(*FindLibrary(s, "L3")) && FindLinkBase(s, "L3") && FindLinkBase(s, "L3")->text == base.text);
    CHECK(base.text.compare(0, 9, "RAVLIB 2\n") == 0 && base.file_by == "Proj.rpp");
    // An edit stays in the project: the base text does not move.
    FindNode(*FindLibrary(s, "L3"), "p1")->volume_db = -2.0;
    CHECK(FindLinkBase(s, "L3")->text == base.text);
    // Unlink: path and base gone, everything else kept.
    CHECK(UnlinkLibraryFromFile(s, "L3"));
    CHECK(!LibraryLinked(*FindLibrary(s, "L3")) && !FindLinkBase(s, "L3") && FindNode(*FindLibrary(s, "L3"), "p1")->volume_db == -2.0);
    CHECK(!UnlinkLibraryFromFile(s, "L3"));
    // Undo = the earlier store text (the base is in it): a link round trip through the chunk keeps it.
    SoundStore t;
    CHECK(ParseSoundStore(before, &t));
    CHECK(LinkLibraryToFile(t, "L3", "Proj.ravlib", base));
    const std::string linked = SerializeSoundStore(t);
    SoundStore back;
    CHECK(ParseSoundStore(linked, &back) && LibraryLinked(*FindLibrary(back, "L3")) && FindLinkBase(back, "L3"));
    // Attached libraries never link; an unknown uid neither.
    SoundLibrary att;
    att.uid = "A1";
    att.attached = true;
    t.libraries.push_back(att);
    CHECK(!LinkLibraryToFile(t, "A1", "A.ravlib", base) && !LinkLibraryToFile(t, "nope", "A.ravlib", base));
    // Locate: another name, the base stays.
    CHECK(RelinkLibraryFile(t, "L3", "Moved.ravlib") && FindLinkBase(t, "L3")->text == base.text);
    CHECK(!RelinkLibraryFile(t, "L3", "Moved.ravlib") && !RelinkLibraryFile(t, "L2", "X.ravlib"));
    // Delete library prunes the base; a base of an unlinked library is not written.
    SoundStore d = t;
    CHECK(DeleteLibrary(d, "L3") && d.link_bases.empty());
    SoundStore e = t;
    e.libraries[2].ravlib_path.clear();
    CHECK(SerializeSoundStore(e).find("linkbase") == std::string::npos);
    CHECK(PruneLinkBases(e) && e.link_bases.empty());
}

static void TestLinkedFileState()
{
    SoundLibrary lib;
    lib.uid = "L1";
    CHECK(LinkedFileStateOf(lib, {}) == LinkedFileState::Unlinked);
    lib.ravlib_path = "Foot.ravlib";
    CHECK(LinkedFileStateOf(lib, {}) == LinkedFileState::Missing);
    CHECK(LinkedFileStateOf(lib, {{"foot.RAVLIB", "L1"}}) == LinkedFileState::Ok);
    CHECK(LinkedFileStateOf(lib, {{"Foot.ravlib", "L9"}}) == LinkedFileState::OtherLibrary);
    CHECK(LinkedFileStateOf(lib, {{"Foot.ravlib", ""}}) == LinkedFileState::Unreadable);
    CHECK(LinkedFileStateOf(lib, {{"Other.ravlib", "L1"}}) == LinkedFileState::Missing);  // by name, not by uid
    lib.attached = true;
    CHECK(LinkedFileStateOf(lib, {{"Foot.ravlib", "L1"}}) == LinkedFileState::Unlinked);
}

static void TestImportRefusal()
{
    const std::vector<FolderRavlib> folder = {{"Steps.ravlib", "L1"}, {"Bad.ravlib", ""}, {"Other.ravlib", "L2"}};
    CHECK(FolderFileOfUid(folder, "L2") == "Other.ravlib");  // refused: that library's file is in the folder
    CHECK(FolderFileOfUid(folder, "L7").empty() && FolderFileOfUid(folder, "").empty());
}

static void TestAddLinkedBase()
{
    // A file whose group, value and node uids clash with the project's: renewed in the project, the common version
    // is the file's library in project uids, the map holds the renewed ones.
    SoundStore file = RichStore();
    SoundStore one = RavlibOf(file, file.libraries[0].uid, nullptr);
    SoundStore proj = RichStore();
    proj.libraries.clear();
    SoundLibrary clash;
    clash.uid = "x000000000000009";
    clash.name = "Clash";
    clash.root = "n000000000000001";  // a node uid the file also uses
    SoundNode cn;
    cn.uid = "n000000000000001";
    cn.type = SoundNodeType::Random;
    clash.nodes.push_back(cn);
    proj.libraries.push_back(clash);
    const std::string text = SerializeRavlib(one, "Other.rpp", 1700000000);
    SoundStore        parsed;
    CHECK(ParseRavlib(text, &parsed));
    std::string                                      uid, why;
    std::vector<std::pair<std::string, std::string>> map;
    LinkBase                                         base;
    CHECK(AddRavlibLibrary(proj, parsed, "Steps.ravlib", &uid, &why, &map, &base));
    const SoundLibrary* lib = FindLibrary(proj, uid);
    CHECK(lib && LibraryLinked(*lib) && lib->ravlib_path == "Steps.ravlib");
    CHECK(base.lib_uid == uid && base.file_by == "Other.rpp" && base.file_at == 1700000000);
    CHECK(base.text == SerializeRavlib(RavlibOf(proj, uid, nullptr)));
    CHECK(lib && lib->root != "n000000000000001");  // renewed
    bool has_root = false;
    for (const auto& kv : map) has_root = has_root || (kv.first == "n000000000000001" && kv.second == lib->root);
    CHECK(has_root);
    for (const auto& kv : map) CHECK(kv.first != kv.second);
    // The map round-trips through the project chunk, with the base.
    base.uid_map = map;
    CHECK(LinkLibraryToFile(proj, uid, "Steps.ravlib", base));
    SoundStore rt;
    CHECK(ParseSoundStore(SerializeSoundStore(proj), &rt));
    const LinkBase* rb = FindLinkBase(rt, uid);
    CHECK(rb && rb->uid_map == map && rb->text == base.text);
    // The header fields are the older readers' unknown fields: a file written by 13-77 reads as before.
    std::string by;
    int64_t     at = 0;
    RavlibWriter(parsed, &by, &at);
    CHECK(by == "Other.rpp" && at == 1700000000);
    CHECK(text.compare(0, 35, "RAVLIB 2 by=Other.rpp at=1700000000") == 0);
}

int main()
{
    TestOldProjectConversion();
    TestLinkUnlink();
    TestLinkedFileState();
    TestImportRefusal();
    TestAddLinkedBase();
    TestAdoptNeedsLink();
    TestAddMapHoldsMerged();
    TestFileDecisions();
    TestFirstSightTargets();
    TestDividedTravels();
    TestPartsTravel();  // 13-62
    TestFileSourceFromItem();
    TestConvertLibrary();
    TestConvertFreezesFrames();
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
