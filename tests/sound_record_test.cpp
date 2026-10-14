// SPDX-License-Identifier: MIT
//
// Host test of the sound model's text (src/sound_record.h, Epic 13 story 13-2): the project
// store, the per-item record, the placed tag, round trips both ways, a future version, a corrupt
// text, the lookups. No REAPER, no Windows: any C++17 compiler.

#include "sound_record.h"
#include "sound_zones.h"  // MigrateLegacyZones (13-62h)

#include <cstdio>
#include <string>

using namespace rav;

namespace {

int g_fails = 0;

#define CHECK(c)                                             \
    do {                                                     \
        if (!(c)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            ++g_fails;                                       \
        }                                                    \
    } while (0)

bool SameFrames(const std::vector<SoundFrame>& a, const std::vector<SoundFrame>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].start != b[i].start || a[i].length != b[i].length || a[i].on != b[i].on || a[i].db != b[i].db ||
            a[i].hand != b[i].hand || a[i].kept != b[i].kept || a[i].fade_in != b[i].fade_in ||
            a[i].fade_out != b[i].fade_out || a[i].fade_in_shape != b[i].fade_in_shape ||
            a[i].fade_out_shape != b[i].fade_out_shape || a[i].cuts != b[i].cuts || a[i].cuts_hand != b[i].cuts_hand ||
            a[i].cut_unsure != b[i].cut_unsure || a[i].syncs != b[i].syncs || a[i].hand_syncs != b[i].hand_syncs || a[i].part_db != b[i].part_db ||
            a[i].part_trims != b[i].part_trims || a[i].part_trim_key != b[i].part_trim_key || a[i].owners != b[i].owners)
            return false;
    return true;
}

// 13-62: a part's own settings.
bool SamePartSettings(const std::vector<PartSettings>& a, const std::vector<PartSettings>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].value != b[i].value || a[i].preroll_ms != b[i].preroll_ms || a[i].tail_ms != b[i].tail_ms ||
            a[i].sync_mode != b[i].sync_mode || a[i].sync_offset_ms != b[i].sync_offset_ms || a[i].gain_db != b[i].gain_db ||
            a[i].norm_mode != b[i].norm_mode || a[i].norm_target != b[i].norm_target || a[i].mono != b[i].mono ||
            a[i].fade_in != b[i].fade_in || a[i].fade_out != b[i].fade_out || a[i].kept != b[i].kept)
            return false;
    return true;
}

// 13-20 / 13-23 / 13-25: a Sound's parts, zones and sync settings.
bool SameParts(const SoundData& x, const SoundData& y)
{
    if (x.divide_group != y.divide_group || x.divide_values != y.divide_values || x.lanes_split != y.lanes_split ||
        x.sync_mode != y.sync_mode || x.sync_offset_ms != y.sync_offset_ms || x.cut_fade != y.cut_fade || x.mono != y.mono ||
        x.fade_in != y.fade_in || x.fade_out != y.fade_out || x.role_map.size() != y.role_map.size() ||
        x.zones.size() != y.zones.size() || !SamePartSettings(x.parts, y.parts))
        return false;
    for (size_t i = 0; i < x.role_map.size(); ++i)
        if (x.role_map[i].role != y.role_map[i].role || x.role_map[i].value != y.role_map[i].value) return false;
    for (size_t i = 0; i < x.zones.size(); ++i)
        if (x.zones[i].start != y.zones[i].start || x.zones[i].length != y.zones[i].length ||
            x.zones[i].to != y.zones[i].to || x.zones[i].kept != y.zones[i].kept)
            return false;
    return true;
}

bool SameNode(const SoundNode& a, const SoundNode& b)
{
    if (a.uid != b.uid || a.parent != b.parent || a.type != b.type || a.name != b.name ||
        a.volume_db != b.volume_db || a.offset_ms != b.offset_ms || a.avoid_repeat != b.avoid_repeat ||
        a.group_uid != b.group_uid || a.default_value != b.default_value || a.preview_value != b.preview_value ||
        a.unassigned != b.unassigned || a.lands_in != b.lands_in || a.kept != b.kept || a.kept_lines != b.kept_lines ||
        a.layer_roles != b.layer_roles || a.layer_keep_offset != b.layer_keep_offset)
        return false;
    if (a.cases.size() != b.cases.size()) return false;
    for (size_t i = 0; i < a.cases.size(); ++i)
        if (a.cases[i].value_uid != b.cases[i].value_uid || a.cases[i].child_uid != b.cases[i].child_uid) return false;
    const SoundData& x = a.sound;
    const SoundData& y = b.sound;
    return x.source.kind == y.source.kind && x.source.item_guid == y.source.item_guid &&
           x.source.asset.name == y.source.asset.name && x.source.asset.size == y.source.asset.size &&
           x.source.asset.length == y.source.asset.length && x.source.asset.last_path == y.source.asset.last_path &&
           x.source.asset.rel_path == y.source.asset.rel_path && x.source.asset.mtime == y.source.asset.mtime &&
           x.mode == y.mode && x.silence.threshold_db == y.silence.threshold_db &&
           x.silence.min_gap_ms == y.silence.min_gap_ms && x.silence.preroll_ms == y.silence.preroll_ms &&
           x.silence.tail_ms == y.silence.tail_ms && x.edited == y.edited && x.linked_copies == y.linked_copies &&
           x.kept == y.kept && SameFrames(x.frames, y.frames) && SameParts(x, y);
}

// 13-62h: what a store reads back as: its divided Sounds with the old model's keys migrated into zones.
SoundStore Migrated(SoundStore s)
{
    for (SoundLibrary& l : s.libraries)
        for (SoundNode& n : l.nodes)
            if (n.type == SoundNodeType::Sound) MigrateLegacyZones(n.sound);
    return s;
}

bool SameStore(const SoundStore& a, const SoundStore& b)
{
    if (a.version != b.version || a.kept != b.kept || a.kept_lines != b.kept_lines) return false;
    if (a.groups.size() != b.groups.size() || a.libraries.size() != b.libraries.size()) return false;
    for (size_t i = 0; i < a.groups.size(); ++i) {
        const SwitchGroup &g = a.groups[i], &h = b.groups[i];
        if (g.uid != h.uid || g.name != h.name || g.default_value != h.default_value || g.kept != h.kept ||
            g.kept_lines != h.kept_lines || g.values.size() != h.values.size())
            return false;
        for (size_t j = 0; j < g.values.size(); ++j)
            if (g.values[j].uid != h.values[j].uid || g.values[j].name != h.values[j].name ||
                g.values[j].kept != h.values[j].kept || g.values[j].roles != h.values[j].roles)
                return false;
    }
    for (size_t i = 0; i < a.libraries.size(); ++i) {
        const SoundLibrary &l = a.libraries[i], &m = b.libraries[i];
        if (l.uid != m.uid || l.name != m.name || l.color != m.color || l.root != m.root || l.user != m.user ||
            l.ravlib_path != m.ravlib_path || l.kept != m.kept || l.kept_lines != m.kept_lines ||
            l.nodes.size() != m.nodes.size())
            return false;
        for (size_t j = 0; j < l.nodes.size(); ++j)
            if (!SameNode(l.nodes[j], m.nodes[j])) return false;
    }
    return true;
}

SoundStore BigStore()
{
    SoundStore s;
    SwitchGroup mat;
    mat.uid = "a000000000000001";
    mat.name = "Material";
    mat.values = {{"a000000000000002", "Wood", {}}, {"a000000000000003", "Wet concrete", {}}};
    mat.default_value = "a000000000000003";  // 13-18
    SwitchGroup spd;
    spd.uid = "a000000000000004";
    spd.name = "Speed=fast %";
    spd.values = {{"a000000000000005", "Walk", {}}};
    s.groups = {mat, spd};

    SoundLibrary steps;
    steps.uid = "b000000000000001";
    steps.name = "Footsteps hero";
    steps.color = 0x1000000u | 0x5B8CFFu;
    steps.root = "c000000000000001";
    SoundNode root;
    root.uid = "c000000000000001";
    root.type = SoundNodeType::Switch;
    root.name = "Root";
    root.group_uid = mat.uid;
    root.default_value = "a000000000000002";
    root.preview_value = "a000000000000003";
    root.cases = {{"a000000000000002", "c000000000000002", {}}, {"a000000000000003", "c000000000000003", {}}};
    root.unassigned = {"c000000000000005"};
    root.volume_db = -3.5;
    SoundNode rnd;
    rnd.uid = "c000000000000002";
    rnd.parent = root.uid;
    rnd.type = SoundNodeType::Random;
    rnd.name = "Wood";
    rnd.avoid_repeat = 3;
    SoundNode blend;
    blend.uid = "c000000000000003";
    blend.parent = root.uid;
    blend.type = SoundNodeType::Blend;
    blend.name = "Concrete";
    SoundNode snd;
    snd.uid = "c000000000000004";
    snd.parent = rnd.uid;
    snd.type = SoundNodeType::Sound;
    snd.name = "wood a";
    snd.offset_ms = 12.5;
    snd.sound.source.kind = SoundSourceKind::Item;
    snd.sound.source.item_guid = "{11111111-2222-3333-4444-555555555555}";
    snd.sound.mode = DetectMode::Silence;
    snd.sound.silence.threshold_db = -48;
    snd.sound.silence.min_gap_ms = 80;
    snd.sound.silence.preroll_ms = 3;
    snd.sound.silence.tail_ms = 120;
    snd.sound.edited = true;
    snd.sound.frames = {{0.0, 0.25, true, 0.0, false, {}}, {0.5, 0.125, false, -2.25, true, {}}};
    // 13-20 / 13-23 / 13-25: divided by Material (with roles on its values), zones, cuts and syncs.
    s.groups[0].values[0].roles = {"heel", "toe_end"};
    s.groups[0].values[1].roles = {"my role"};  // encoded: a custom key never holds a space, the grammar copes
    snd.sound.divide_group = mat.uid;
    snd.sound.divide_values = {"a000000000000002", "a000000000000003"};
    snd.sound.role_map = {{"heel", "a000000000000003"}, {"hand", "a000000000000002"}};
    snd.sound.lanes_split = true;
    snd.sound.sync_mode = SyncMode::SliceStart;
    snd.sound.sync_offset_ms = 1.5;
    snd.sound.cut_fade = 0.004;
    snd.sound.mono = MonoMode::Right;  // 13-41
    snd.sound.zones = {{0.05, 0.01, "", {}}, {0.1, 0.02, "a000000000000002", {}}};
    snd.sound.frames[0].cuts = {0.125};
    snd.sound.frames[0].cuts_hand = true;
    snd.sound.frames[0].syncs = {0.018, 0.13};
    snd.sound.frames[0].hand_syncs = {0.13};
    snd.sound.frames[1].cuts = {0.5625};
    snd.sound.frames[1].cut_unsure = true;
    snd.sound.frames[1].syncs = {-1, 0.57};
    SoundNode file;
    file.uid = "c000000000000005";
    file.parent = root.uid;
    file.type = SoundNodeType::Sound;
    file.sound.source.kind = SoundSourceKind::File;
    file.sound.source.asset = {"Step 01.WAV", 123456789012LL, 1.5, "C:\\SFX\\Step 01.WAV", "media/Step 01.WAV", 1700000000};
    file.sound.linked_copies = false;
    file.sound.mode = DetectMode::Takes;
    file.lands_in = "Foley/Steps";
    file.layer_roles = {"heel", "toe_end"};  // 13-31
    file.layer_keep_offset = true;
    steps.nodes = {root, rnd, blend, snd, file};

    SoundLibrary user;
    user.uid = "b000000000000002";
    user.name = "Mine";
    user.root = "d000000000000001";
    user.user = true;
    user.ravlib_path = "C:\\libs\\mine.ravlib";
    SoundNode uroot;
    uroot.uid = "d000000000000001";
    uroot.type = SoundNodeType::Random;
    uroot.sound.mode = DetectMode::TakeMarkers;  // sound data on a non-Sound node (a type change) is kept
    user.nodes = {uroot};
    s.libraries = {steps, user};
    return s;
}

}  // namespace

int main()
{
    // ---- Store: round trip both ways -------------------------------------------------------------
    {
        const SoundStore  s = BigStore();
        const std::string text = SerializeSoundStore(s);
        CHECK(text.compare(0, 12, "RAVSOUNDS 1\n") == 0);
        SoundStore p;
        CHECK(ParseSoundStore(text, &p));
        // 13-62h: the divided Sound with the old model's keys reads back as zones.
        CHECK(SameStore(p, Migrated(s)));
        CHECK(SerializeSoundStore(p) == SerializeSoundStore(Migrated(s)));
        CHECK(!p.libraries[0].nodes[3].sound.frames[0].zones.empty() && p.libraries[0].nodes[3].sound.frames[0].cuts.empty());
        SoundStore p2;
        CHECK(ParseSoundStore(SerializeSoundStore(p), &p2) && SerializeSoundStore(p2) == SerializeSoundStore(p));
        // Strings are encoded: no raw space inside a value.
        CHECK(text.find("name=Wet%20concrete") != std::string::npos);
        CHECK(text.find("name=Speed%3Dfast%20%25") != std::string::npos);
        CHECK(p.groups[1].name == "Speed=fast %");
        CHECK(p.libraries[0].nodes[4].sound.source.asset.size == 123456789012LL);
        CHECK(p.libraries[0].nodes[3].sound.silence.min_gap_ms == 80 && p.libraries[0].nodes[3].sound.silence.preroll_ms == 3 &&
              p.libraries[0].nodes[3].sound.silence.tail_ms == 120);
        CHECK(p.libraries[0].nodes[4].sound.mode == DetectMode::Takes);
        CHECK(p.libraries[1].nodes[0].sound.mode == DetectMode::TakeMarkers);
        CHECK(text.find(" mode=takes") != std::string::npos && text.find(" mode=tmarkers") != std::string::npos);
        // A group line after a library: the following node lines are not the earlier library's.
        SoundStore g;
        CHECK(ParseSoundStore("RAVSOUNDS 1\nlibrary uid=l name=L root=n\nnode uid=n type=switch\n"
                              "group uid=g name=G\ncase value=v child=c\n", &g));
        CHECK(g.libraries.size() == 1 && g.libraries[0].nodes[0].cases.empty() && g.groups[0].kept_lines.size() == 1);
    }
    // ---- Store: unknown lines and fields kept, written back in place -----------------------------
    {
        SoundStore s = Migrated(BigStore());  // 13-62h: as a project of this version writes it
        std::string text = SerializeSoundStore(s);
        // An unknown field at the end of a node line, an unknown line after a node, after the header.
        const std::string node_line = "node uid=c000000000000002 type=random parent=c000000000000001 name=Wood avoid=3\n";
        CHECK(text.find(node_line) != std::string::npos);
        std::string odd = text;
        odd.replace(odd.find(node_line), node_line.size(),
                    "node uid=c000000000000002 type=random parent=c000000000000001 name=Wood avoid=3 mood=calm\n"
                    "sparkle level=3\n");
        odd.insert(std::string("RAVSOUNDS 1\n").size(), "futureheader x=1\n");
        SoundStore p;
        CHECK(ParseSoundStore(odd, &p));
        CHECK(SerializeSoundStore(p) == odd);
        CHECK(p.kept_lines.size() == 1 && p.kept_lines[0] == "futureheader x=1");
        const SoundNode* n = FindNode(p.libraries[0], "c000000000000002");
        CHECK(n && n->kept.size() == 1 && n->kept[0] == "mood=calm");
        CHECK(n && n->kept_lines.size() == 1 && n->kept_lines[0] == "sparkle level=3");
        // parse(write(x)) == x with kept text too.
        SoundStore q;
        CHECK(ParseSoundStore(SerializeSoundStore(p), &q) && SameStore(p, q));
    }
    // ---- 13-18: a group's default value (written only when set) ------------------------------------
    {
        SoundStore d;
        CHECK(ParseSoundStore("RAVSOUNDS 1\ngroup uid=g name=G default=v2\nvalue uid=v1 name=A\nvalue uid=v2 name=B\n", &d));
        CHECK(d.groups.size() == 1 && d.groups[0].default_value == "v2" && d.groups[0].kept.empty());
        CHECK(SerializeSoundStore(d).find("group uid=g name=G default=v2\n") != std::string::npos);
        d.groups[0].default_value.clear();
        CHECK(SerializeSoundStore(d).find("group uid=g name=G\n") != std::string::npos);
    }
    // ---- Empty store: no chunk -------------------------------------------------------------------
    {
        SoundStore e;
        CHECK(SoundStoreEmpty(e));
        CHECK(SerializeSoundStore(e).empty());
        SoundStore p;
        CHECK(ParseSoundStore("RAVSOUNDS 1\n", &p) && SoundStoreEmpty(p) && SerializeSoundStore(p).empty());
    }
    // ---- Future version: read best-effort, kept --------------------------------------------------
    {
        const std::string fut = "RAVSOUNDS 2 flavour=new\n"
                                "group uid=g1 name=Surface\n"
                                "value uid=v1 name=Grass\n"
                                "hologram uid=h1\n"
                                "library uid=l1 name=Lib root=n1\n"
                                "node uid=n1 type=random\n"
                                "node uid=n2 type=teleport parent=n1\n";
        SoundStore p;
        CHECK(ParseSoundStore(fut, &p));
        CHECK(p.version == 2 && p.groups.size() == 1 && p.groups[0].values.size() == 1 && p.libraries.size() == 1);
        CHECK(p.libraries[0].nodes.size() == 2);
        const std::string back = SerializeSoundStore(p);
        CHECK(back.compare(0, 24, "RAVSOUNDS 2 flavour=new\n") == 0);
        CHECK(back.find("hologram uid=h1\n") != std::string::npos);
        CHECK(back.find("node uid=n2 parent=n1 type=teleport\n") != std::string::npos);  // kept as written
        const std::string fut_known = fut.substr(0, fut.find("node uid=n2"));
        CHECK(back.compare(0, fut_known.size(), fut_known) == 0);
    }
    // ---- Corrupt text: false, output untouched ---------------------------------------------------
    {
        SoundStore keep = BigStore();
        const std::string before = SerializeSoundStore(keep);
        CHECK(!ParseSoundStore("hello\nRAVSOUNDS 1\n", &keep));
        CHECK(!ParseSoundStore("", &keep));
        CHECK(!ParseSoundStore("RAVSOUNDS x\n", &keep));
        CHECK(!ParseSoundStore("RAVSOUNDS 0\n", &keep));
        CHECK(!ParseSoundStore("RAVITEMSND 1\n", &keep));
        CHECK(!ParseSoundStore("RAVSOUNDS 1\n", nullptr));
        CHECK(SerializeSoundStore(keep) == before);
        // Leading blank lines and a BOM are allowed.
        SoundStore b;
        CHECK(ParseSoundStore("\xEF\xBB\xBF\n\nRAVSOUNDS 1\r\ngroup uid=g name=G\r\n", &b) && b.groups.size() == 1);
    }
    // ---- Item record -----------------------------------------------------------------------------
    {
        ItemSounds s;
        s.seed = 1234567890123ULL;
        s.item_values = {{"g1", "v1", {}}};
        RuleSounds r;
        r.rid = "0123456789abcdef";
        r.library = "b000000000000001";
        r.values = {{"g2", "v2", {}}};
        RuleSounds r2;
        r2.rid = "fedcba9876543210";
        s.rules = {r, r2};
        EventSounds e;
        e.rid = r.rid;
        e.t = 1.25;
        e.overrides = {{"g1", "v3", {}}};
        e.draws = {{"c000000000000002", 2, true, {}}, {"c000000000000004", 0, false, {}}};
        SoundPlacement pl;
        pl.item_guid = "{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}";
        pl.node = "c000000000000004";
        pl.own = Ownership::Attached;
        pl.offset_s = -0.015;
        pl.variation = 1;
        pl.track_guid = "{TTTT}";
        SoundPlacement gone;
        gone.item_guid = "{DEAD}";
        gone.node = "c000000000000004";
        gone.own = Ownership::Deleted;
        e.placed = {pl, gone};
        EventSounds e2;
        e2.rid = r2.rid;
        e2.t = 3.5;
        s.events = {e, e2};
        const std::string text = SerializeItemSounds(s);
        CHECK(text.compare(0, 31, "RAVITEMSND 1 seed=1234567890123") == 0);
        ItemSounds p;
        CHECK(ParseItemSounds(text, &p));
        CHECK(SerializeItemSounds(p) == text);
        CHECK(p.seed == s.seed && p.item_values.size() == 1 && p.rules.size() == 2 && p.events.size() == 2);
        CHECK(p.rules[0].values.size() == 1 && p.rules[0].values[0].value_uid == "v2" && p.rules[1].library.empty());
        CHECK(p.events[0].overrides.size() == 1 && p.events[0].draws.size() == 2 && p.events[0].placed.size() == 2);
        CHECK(p.events[0].draws[0].pick == 2 && p.events[0].draws[0].hand && !p.events[0].draws[1].hand);
        CHECK(p.events[0].placed[0].own == Ownership::Attached && p.events[0].placed[0].offset_s == -0.015 &&
              p.events[0].placed[0].variation == 1 && p.events[0].placed[0].track_guid == "{TTTT}");
        CHECK(p.events[0].placed[1].own == Ownership::Deleted);
        // Unknown lines and fields kept.
        std::string odd = text + "future thing=1\n";
        odd.replace(odd.find("event rid=fedcba9876543210 t=3.5\n"), 34, "event rid=fedcba9876543210 t=3.5 glow=1\n");
        ItemSounds q;
        CHECK(ParseItemSounds(odd, &q) && SerializeItemSounds(q) == odd);
        CHECK(q.events[1].kept.size() == 1 && q.events[1].kept_lines.size() == 1);
        // Lookups: an event within +-30 ms.
        CHECK(FindEventSounds(p, r.rid, 1.27) == &p.events[0]);
        CHECK(FindEventSounds(p, r.rid, 1.29) == nullptr);
        CHECK(FindEventSounds(p, r2.rid, 1.25) == nullptr);
        CHECK(FindRuleSounds(p, r2.rid) == &p.rules[1] && FindRuleSounds(p, "x") == nullptr);
        // Empty / corrupt.
        CHECK(ItemSoundsEmpty(ItemSounds{}) && !ItemSoundsEmpty(p));
        ItemSounds keep = p;
        CHECK(!ParseItemSounds("RAVSOUNDS 1\n", &keep) && SerializeItemSounds(keep) == text);
    }
    // ---- Placed tag ------------------------------------------------------------------------------
    {
        PlacedTag t;
        t.owner = "{01234567-89AB-CDEF-0123-456789ABCDEF}";
        t.rid = "0123456789abcdef";
        t.t = 0.5;
        t.node = "c000000000000004";
        t.own = Ownership::Spread;
        const std::string text = SerializePlacedTag(t);
        CHECK(text == "RAVSND 2 owner={01234567-89AB-CDEF-0123-456789ABCDEF} rid=0123456789abcdef t=0.5 "
                      "node=c000000000000004 own=spread\n");
        PlacedTag p;
        CHECK(ParsePlacedTag(text, &p) && p.owner == t.owner && p.rid == t.rid && p.t == t.t && p.node == t.node &&
              p.own == t.own);
        CHECK(SerializePlacedTag(p) == text);
        PlacedTag keep = p;
        CHECK(!ParsePlacedTag("garbage", &keep));
        CHECK(!ParsePlacedTag("RAVSND 1 rid=x\n", &keep));  // no owner
        CHECK(!ParsePlacedTag("", &keep));
        CHECK(keep.owner == t.owner);
        // A higher version and unknown fields survive a write.
        const std::string fut = "RAVSND 3 owner={O} rid=0123456789abcdef t=1 node=n own=populated glow=1\n";
        PlacedTag f;
        // 13-8: the pile anchor (ao / sy) round-trips; a v1 tag reads without it and stays v1.
        {
            PlacedTag a = t;
            a.has_anchor = true;
            a.ao = -0.25;
            a.sy = 0.4;
            const std::string at = SerializePlacedTag(a);
            CHECK(at.find(" ao=-0.25 sy=0.4") != std::string::npos);
            PlacedTag b;
            CHECK(ParsePlacedTag(at, &b) && b.has_anchor && b.ao == -0.25 && b.sy == 0.4 && SerializePlacedTag(b) == at);
            const std::string v1 = "RAVSND 1 owner={O} rid=r t=1 node=n own=populated\n";
            PlacedTag c;
            CHECK(ParsePlacedTag(v1, &c) && !c.has_anchor && c.version == 1 && SerializePlacedTag(c) == v1);
            PlacedTag half;
            CHECK(ParsePlacedTag("RAVSND 2 owner={O} ao=1\n", &half) && !half.has_anchor);  // ao without sy: unused
        }
        // 13-16: the placed-on track mark (with spaces and |) round-trips; none = no field.
        {
            PlacedTag a = t;
            a.tk = "PT|RAV Library|lib|g:v|Y|heel|S|2";
            const std::string at = SerializePlacedTag(a);
            PlacedTag b;
            CHECK(ParsePlacedTag(at, &b) && b.tk == a.tk && SerializePlacedTag(b) == at);
            CHECK(SerializePlacedTag(t).find("tk=") == std::string::npos);
        }
        CHECK(ParsePlacedTag(fut, &f) && f.version == 3 && !f.has_anchor && f.kept.size() == 1 && SerializePlacedTag(f) == fut);
    }
    // ---- Lookups in the store --------------------------------------------------------------------
    {
        SoundStore s = BigStore();
        const SoundLibrary* lib = FindLibrary(s, "b000000000000001");
        CHECK(lib && FindLibrary(s, "nope") == nullptr);
        CHECK(FindGroup(s, "a000000000000004") == &s.groups[1]);
        CHECK(lib && FindNode(*lib, "c000000000000003") == &lib->nodes[2] && FindNode(*lib, "") == nullptr);
        const std::vector<const SoundNode*> kids = ChildrenOf(*lib, "c000000000000001");
        CHECK(kids.size() == 3 && kids[0]->uid == "c000000000000002" && kids[2]->uid == "c000000000000005");
        const std::vector<const SoundNode*> top = ChildrenOf(*lib, "");
        CHECK(top.size() == 1 && top[0]->uid == lib->root);
        CHECK(std::string(SoundNodeTypeWord(SoundNodeType::Blend)) == "blend");
        Ownership o;
        CHECK(OwnershipFromWord("attached", &o) && o == Ownership::Attached && !OwnershipFromWord("x", &o));
    }
    // ---- Fades (Antho 2026-10-12): per frame and the Sound's default, optional fields -----------
    {
        // Old text (no fade field): the defaults (1 / 5 ms, no frame fade), and nothing is written.
        SoundStore old;
        CHECK(ParseSoundStore("RAVSOUNDS 1\nlibrary uid=l name=L root=n\nnode uid=n type=sound\nsound kind=item guid=g\n"
                              "frame start=0 len=1\n", &old));
        const SoundData& od = old.libraries[0].nodes[0].sound;
        CHECK(od.fade_in == kDefaultFadeIn && od.fade_out == kDefaultFadeOut);
        CHECK(od.frames.size() == 1 && od.frames[0].fade_in < 0.0 && od.frames[0].fade_out < 0.0 &&
              od.frames[0].fade_in_shape == 0 && od.frames[0].fade_out_shape == 0);
        const std::string ot = SerializeSoundStore(old);
        CHECK(ot.find("fin=") == std::string::npos && ot.find("fout=") == std::string::npos &&
              ot.find("fish=") == std::string::npos);
        // Set ones round-trip.
        SoundStore s = old;
        SoundData& d = s.libraries[0].nodes[0].sound;
        d.fade_in = 0.0;
        d.fade_out = 0.012;
        d.frames[0].fade_in = 0.002;
        d.frames[0].fade_out = 0.0;  // a hand-set 0 (no fade) is not the default
        d.frames[0].fade_in_shape = 1;
        d.frames[0].fade_out_shape = 5;
        const std::string t = SerializeSoundStore(s);
        CHECK(t.find("sound kind=item guid=g fin=0 fout=0.012") != std::string::npos);
        CHECK(t.find("frame start=0 len=1 fin=0.002 fout=0 fish=1 fosh=5") != std::string::npos);
        SoundStore b;
        CHECK(ParseSoundStore(t, &b));
        const SoundData& bd = b.libraries[0].nodes[0].sound;
        CHECK(bd.fade_in == 0.0 && bd.fade_out == 0.012);
        CHECK(bd.frames[0].fade_in == 0.002 && bd.frames[0].fade_out == 0.0 && bd.frames[0].fade_in_shape == 1 &&
              bd.frames[0].fade_out_shape == 5);
        CHECK(SerializeSoundStore(b) == t);
        // A bad value is kept as text and stands for the field (the default meanwhile).
        SoundStore bad;
        CHECK(ParseSoundStore("RAVSOUNDS 1\nlibrary uid=l name=L root=n\nnode uid=n type=sound\nsound kind=item\n"
                              "frame start=0 len=1 fin=abc\n", &bad));
        CHECK(bad.libraries[0].nodes[0].sound.frames[0].fade_in < 0.0);
    }

    // ---- 13-28: a part's own gain (pdb.<value uid>), none written for a frame without ----------------
    {
        SoundStore s = BigStore();
        SoundFrame& f = s.libraries[0].nodes[3].sound.frames[1];
        f.part_db["a000000000000002"] = 3.5;
        f.part_db["a 3"] = -1.25;  // a uid that needs escaping
        const std::string t = SerializeSoundStore(s);
        CHECK(t.find(" pdb.a%203=-1.25 pdb.a000000000000002=3.5\n") != std::string::npos);
        CHECK(t.find("frame start=0 len=0.25 cuts=0.125 cutsh=1 sync=0.018,0.13 hsync=0.13\n") != std::string::npos);
        SoundStore p;
        CHECK(ParseSoundStore(t, &p) && SameStore(p, Migrated(s)) && SerializeSoundStore(p) == SerializeSoundStore(Migrated(s)));
        // 13-62h: read back as zones: frame 1's Wood zones carry Wood's part gain, no part_db kept.
        const SoundFrame& pf = p.libraries[0].nodes[3].sound.frames[1];
        bool              wood = false;
        for (const EditZone& z : pf.zones)
            if (z.part == "a000000000000002") wood = z.db == 3.5;
        CHECK(pf.part_db.empty() && wood);
        CHECK(p.libraries[0].nodes[3].sound.frames[0].part_db.empty());
        // A pdb that does not read is kept as written.
        SoundStore bad;
        CHECK(ParseSoundStore("RAVSOUNDS 1\nlibrary uid=l name=L root=n\nnode uid=n type=sound\nsound kind=item\n"
                              "frame start=0 len=1 pdb.x=abc\n", &bad));
        CHECK(bad.libraries[0].nodes[0].sound.frames[0].part_db.empty() &&
              SerializeSoundStore(bad).find(" pdb.x=abc\n") != std::string::npos);
    }

    // ---- 13-62: a part's own settings (`part` lines after the sound line) and the pieces' owners ------
    {
        SoundStore s = BigStore();
        SoundData& d = s.libraries[0].nodes[3].sound;
        PartSettings toe;
        toe.value = "a000000000000003";
        toe.preroll_ms = 12.0;
        toe.sync_mode = SyncMode::Attack;  // own even when it equals the Sound's default: written
        toe.gain_db = -3.0;
        toe.norm_mode = 3;
        toe.norm_target = -23.0;
        toe.mono = MonoMode::Left;
        toe.fade_in = 0.004;
        PartSettings heel;
        heel.value = "a 2";  // a uid that needs escaping
        heel.tail_ms = 40.0;
        heel.sync_offset_ms = 0.0;
        heel.mono = MonoMode::Off;
        heel.fade_out = 0.01;
        d.parts = {toe, heel};
        d.frames[0].owners = {"a000000000000002", "a 2"};
        const std::string t = SerializeSoundStore(s);
        CHECK(t.find(" mono=right\npart v=a000000000000003 pre=12 sync=attack gain=-3 nmode=3 ntgt=-23 mono=left fin=0.004\n"
                     "part v=a%202 tail=40 soff=0 mono=off fout=0.01\nframe start=0 ") != std::string::npos);
        CHECK(t.find(" hsync=0.13 own=a000000000000002,a%202\n") != std::string::npos);
        SoundStore p;
        CHECK(ParseSoundStore(t, &p) && SameStore(p, Migrated(s)) && SerializeSoundStore(p) == SerializeSoundStore(Migrated(s)));
        const SoundData& q = p.libraries[0].nodes[3].sound;
        CHECK(q.parts.size() == 2 && q.parts[1].value == "a 2" && q.parts[1].mono == MonoMode::Off && !q.parts[1].gain_db &&
              q.parts[0].sync_mode == SyncMode::Attack && !q.parts[0].tail_ms);
        // 13-62h: the owners are read (not sane here: "a 2" is no divide value) and become zones.
        CHECK(q.frames[0].owners.empty() && !q.frames[0].zones.empty() && q.frames[1].owners.empty());
        // Nothing of the kind: no part line, no own field (the text of a Sound without them is unchanged).
        const std::string plain = SerializeSoundStore(BigStore());
        CHECK(plain.find("\npart ") == std::string::npos && plain.find(" own=") == std::string::npos);
        // Unknown fields of a part line and values that do not read are kept as written.
        SoundStore k;
        CHECK(ParseSoundStore("RAVSOUNDS 1\nlibrary uid=l name=L root=n\nnode uid=n type=sound\nsound kind=item\n"
                              "part v=a gain=x future=1 mono=sum\nframe start=0 len=1\n", &k));
        const SoundData& kd = k.libraries[0].nodes[0].sound;
        CHECK(kd.parts.size() == 1 && kd.parts[0].mono == MonoMode::Sum && !kd.parts[0].gain_db && kd.frames.size() == 1);
        CHECK(SerializeSoundStore(k).find("part v=a mono=sum gain=x future=1\nframe start=0 len=1\n") != std::string::npos);
        // 13-62 review: SetPartSetting's bounds on read: an unknown Normalize mode and a non-finite number are
        // kept as written (not read); negative margins / fades read as 0, the target clamped to -60..0 dB.
        SoundStore b;
        CHECK(ParseSoundStore("RAVSOUNDS 1\nlibrary uid=l name=L root=n\nnode uid=n type=sound\nsound kind=item\n"
                              "part v=a nmode=6 gain=inf pre=-5 tail=-1 fin=-0.1 fout=-2 ntgt=-80\n"
                              "part v=b nmode=5 ntgt=3\nframe start=0 len=1\n",
                              &b));
        const SoundData& bd = b.libraries[0].nodes[0].sound;
        CHECK(bd.parts.size() == 2 && !bd.parts[0].norm_mode && !bd.parts[0].gain_db && *bd.parts[0].preroll_ms == 0.0 &&
              *bd.parts[0].tail_ms == 0.0 && *bd.parts[0].fade_in == 0.0 && *bd.parts[0].fade_out == 0.0 &&
              *bd.parts[0].norm_target == -60.0);
        CHECK(*bd.parts[1].norm_mode == 5 && *bd.parts[1].norm_target == 0.0);
        const std::string bt = SerializeSoundStore(b);
        CHECK(bt.find("part v=a pre=0 tail=0 ntgt=-60 fin=0 fout=0 nmode=6 gain=inf\n") != std::string::npos);
    }

    // ---- 13-54: a frame's part trims (ptrim), none written for a frame without -----------------------
    {
        SoundStore s = BigStore();
        s.libraries[0].nodes[3].sound.frames[1].part_trims = {0.5, 0.55, 0.5625, 0.625};
        const std::string t = SerializeSoundStore(s);
        CHECK(t.find(" sync=-1,0.57 ptrim=0.5,0.55,0.5625,0.625\n") != std::string::npos);
        CHECK(t.find("hsync=0.13\nframe start=0.5") != std::string::npos);  // frame 0: none
        SoundStore p;
        CHECK(ParseSoundStore(t, &p) && SameStore(p, Migrated(s)) && SerializeSoundStore(p) == SerializeSoundStore(Migrated(s)));
        // 13-54 review: the key they were measured with (ptrimk, encoded), kept through a round trip.
        s.libraries[0].nodes[3].sound.frames[1].part_trim_key = "-45000000,5000000,100000000,00ff00ff00ff00ff|132,4096";
        const std::string tk = SerializeSoundStore(s);
        CHECK(tk.find(" ptrim=0.5,0.55,0.5625,0.625 ptrimk=-45000000%2C5000000%2C100000000%2C00ff00ff00ff00ff|132%2C4096\n") !=
              std::string::npos);
        SoundStore pk;
        CHECK(ParseSoundStore(tk, &pk) && SameStore(pk, Migrated(s)) && SerializeSoundStore(pk) == SerializeSoundStore(Migrated(s)));
    }

    // ---- 13-20 / 13-23 / 13-25: parts, zones, sync points, roles, a layer's / draw's / tag's part ---
    {
        const SoundStore  s = BigStore();
        const std::string t = SerializeSoundStore(s);
        CHECK(t.find("value uid=a000000000000002 name=Wood roles=heel,toe_end\n") != std::string::npos);
        CHECK(t.find("value uid=a000000000000003 name=Wet%20concrete roles=my%20role\n") != std::string::npos);
        CHECK(t.find(" div=a000000000000001 divv=a000000000000002,a000000000000003 "
                     "rmap=heel:a000000000000003,hand:a000000000000002 lanes=split sync=start soff=1.5 cfade=0.004 mono=right\n") !=
              std::string::npos);
        CHECK(t.find("frame start=0 len=0.25 cuts=0.125 cutsh=1 sync=0.018,0.13 hsync=0.13\n") != std::string::npos);
        CHECK(t.find("frame start=0.5 len=0.125 on=0 db=-2.25 hand=1 cuts=0.5625 cutq=1 sync=-1,0.57\n") !=
              std::string::npos);
        // Zones after the Sound's frames, `to` only when set.
        CHECK(t.find("hsync=0.13\nframe start=0.5") != std::string::npos);
        CHECK(t.find("cutq=1 sync=-1,0.57\nzone start=0.05 len=0.01\nzone start=0.1 len=0.02 to=a000000000000002\n") !=
              std::string::npos);
        CHECK(t.find(" lands=Foley%2FSteps roles=heel,toe_end koff=1\n") != std::string::npos ||
              t.find(" lands=Foley/Steps roles=heel,toe_end koff=1\n") != std::string::npos);  // 13-31
        SoundStore p;
        CHECK(ParseSoundStore(t, &p) && SameStore(p, Migrated(s)) && SerializeSoundStore(p) == SerializeSoundStore(Migrated(s)));
        {  // 13-31: the retired 13-23 layer part (part=*) is read and dropped, never kept
            std::string old_t = t;
            const size_t at = old_t.find(" koff=1\n");
            CHECK(at != std::string::npos);
            if (at != std::string::npos) old_t.insert(at, " part=*");
            SoundStore q;
            CHECK(old_t != t && ParseSoundStore(old_t, &q) && SerializeSoundStore(q) == SerializeSoundStore(Migrated(s)));
            CHECK(SerializeSoundStore(q).find("part=") == std::string::npos);
        }
        const SoundData& d = p.libraries[0].nodes[3].sound;
        // 13-62h: the Sound-level zones are read and folded into the zones of its variations.
        CHECK(d.sync_mode == SyncMode::SliceStart && d.zones.empty() && !d.frames[0].zones.empty());
        CHECK(d.role_map.size() == 2 && d.role_map[1].role == "hand");
        CHECK(p.groups[1 - 1].values[1].roles == std::vector<std::string>{"my role"});
        // Each sync word reads; defaults are not written (an old Sound line is unchanged).
        for (SyncMode m : {SyncMode::Attack, SyncMode::Peak, SyncMode::SliceStart}) {
            SoundStore q = s;
            q.libraries[0].nodes[3].sound.sync_mode = m;
            SoundStore r;
            CHECK(ParseSoundStore(SerializeSoundStore(q), &r) && r.libraries[0].nodes[3].sound.sync_mode == m);
        }
        // 13-41: each mono word reads back; Off is never written; an unknown word is refused.
        for (MonoMode m : {MonoMode::Off, MonoMode::Left, MonoMode::Right, MonoMode::Sum}) {
            SoundStore q = s;
            q.libraries[0].nodes[3].sound.mono = m;
            SoundStore r;
            const std::string t = SerializeSoundStore(q);
            CHECK(ParseSoundStore(t, &r) && r.libraries[0].nodes[3].sound.mono == m);
            CHECK((t.find(" mono=") == std::string::npos) == (m == MonoMode::Off));
        }
        {
            SoundStore bad;
            CHECK(!ParseSoundStore("RAVSOUNDS 1\nlibrary uid=l name=L root=n\nnode uid=n type=sound\nsound kind=item guid=g mono=both\n", &bad) ||
                  bad.libraries[0].nodes[0].sound.mono == MonoMode::Off);
        }
        SoundStore old;
        CHECK(ParseSoundStore("RAVSOUNDS 1\nlibrary uid=l name=L root=n\nnode uid=n type=sound\nsound kind=item guid=g\n"
                              "frame start=0 len=1\n", &old));
        const SoundData& od = old.libraries[0].nodes[0].sound;
        CHECK(od.divide_group.empty() && od.zones.empty() && od.sync_mode == SyncMode::Attack &&
              od.sync_offset_ms == kDefaultSyncOffsetMs && od.cut_fade == kDefaultCutFade && !od.lanes_split &&
              od.mono == MonoMode::Off);
        CHECK(od.frames[0].cuts.empty() && od.frames[0].syncs.empty() && !od.frames[0].cuts_hand);
        CHECK(SerializeSoundStore(old) == "RAVSOUNDS 1\nlibrary uid=l name=L root=n\nnode uid=n type=sound\n"
                                          "sound kind=item guid=g\nframe start=0 len=1\n");
        // A zone line on a node without a Sound line still reads (it makes the Sound line written).
        SoundStore z;
        CHECK(ParseSoundStore("RAVSOUNDS 1\nlibrary uid=l name=L root=n\nnode uid=n type=random\n"
                              "zone start=1 len=2 glow=1\n", &z));
        CHECK(z.libraries[0].nodes[0].sound.zones.size() == 1 && z.libraries[0].nodes[0].sound.zones[0].kept.size() == 1);
        CHECK(SerializeSoundStore(z).find("zone start=1 len=2 glow=1\n") != std::string::npos);
        // A bad list is kept as text.
        SoundStore bad;
        CHECK(ParseSoundStore("RAVSOUNDS 1\nlibrary uid=l name=L root=n\nnode uid=n type=sound\nsound kind=item sync=sideways\n"
                              "frame start=0 len=1 cuts=0.5,x\n", &bad));
        CHECK(bad.libraries[0].nodes[0].sound.sync_mode == SyncMode::Attack &&
              bad.libraries[0].nodes[0].sound.kept.size() == 1 && bad.libraries[0].nodes[0].sound.frames[0].cuts.empty() &&
              bad.libraries[0].nodes[0].sound.frames[0].kept.size() == 1);
        // A draw's part and a tag's part.
        ItemSounds it;
        EventSounds e;
        e.rid = "r";
        e.t = 1;
        SoundDraw dr;
        dr.node = "n";
        dr.pick = 2;
        dr.part = "a000000000000003";
        SoundDraw whole;
        whole.node = "n";
        whole.pick = 0;
        e.draws = {dr, whole};
        it.events = {e};
        const std::string it_text = SerializeItemSounds(it);
        CHECK(it_text.find("draw node=n pick=2 part=a000000000000003\ndraw node=n pick=0\n") != std::string::npos);
        ItemSounds ib;
        CHECK(ParseItemSounds(it_text, &ib) && ib.events[0].draws[0].part == "a000000000000003" &&
              ib.events[0].draws[1].part.empty() && SerializeItemSounds(ib) == it_text);
        PlacedTag tag;
        tag.owner = "{O}";
        tag.rid = "r";
        tag.node = "n";
        tag.pt = "a000000000000002";
        const std::string tt = SerializePlacedTag(tag);
        CHECK(tt.find(" pt=a000000000000002\n") != std::string::npos);
        PlacedTag tb;
        CHECK(ParsePlacedTag(tt, &tb) && tb.pt == tag.pt && SerializePlacedTag(tb) == tt);
        tag.pt.clear();
        CHECK(SerializePlacedTag(tag).find("pt=") == std::string::npos);
    }

    if (g_fails) {
        std::printf("sound_record_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("sound_record_test: all passed\n");
    return 0;
}
