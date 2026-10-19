// SPDX-License-Identifier: MIT
//
// Host test of src/sound_spread_plan.h (13-68 v2 / v3): the selection's piles, the library nodes (Random /
// Sound / Takes Sound / Blend + offsets, sync from the item snap), the Attached library (found, replaced,
// Add to stack), the source zone, the report and the attached flag in the store text.

#include "sound_spread_plan.h"

#include <cmath>
#include <cstdio>
#include <string>

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
static bool Near(double a, double b, double e = 1e-9) { return std::fabs(a - b) <= e; }

static SpreadSource Src(const char* guid, const char* name, int track, double pos, double len, double snap, int takes = 1)
{
    SpreadSource s;
    s.facts.guid = guid;
    s.facts.track = track;
    s.facts.pos = pos;
    s.facts.len = len;
    s.facts.snap = snap;
    s.facts.takes = takes;
    s.name = name;
    s.sound.source.kind = SoundSourceKind::Item;
    s.sound.source.item_guid = guid;
    if (takes >= 2) {
        s.sound.mode = DetectMode::Takes;
        for (int k = 0; k < takes; ++k) s.sound.frames.push_back(SoundFrame{});
    }
    return s;
}

static int g_uid = 0;
static std::string Uid()
{
    char b[32];
    std::snprintf(b, sizeof(b), "u%02d", ++g_uid);
    return b;
}

static int ChildrenOf(const std::vector<SoundNode>& nodes, const std::string& parent)
{
    int n = 0;
    for (const SoundNode& x : nodes) n += x.parent == parent ? 1 : 0;
    return n;
}

int main()
{
    // Takes (Antho's case): one item, 4 takes, snap 30 ms: Random[Takes Sound], synced on the snap.
    {
        const std::vector<SoundNode> n = SpreadLayerNodes({Src("k", "Step", 2, 50.0, 0.4, 0.03, 4)}, "FS L", &Uid);
        CHECK(n.size() == 2);
        CHECK(n[0].type == SoundNodeType::Random && n[0].name == "FS L" && n[0].parent.empty());
        CHECK(n[1].type == SoundNodeType::Sound && n[1].parent == n[0].uid && n[1].name == "Step");
        CHECK(n[1].sound.mode == DetectMode::Takes && n[1].sound.frames.size() == 4);
        CHECK(n[1].sound.source.item_guid == "k");
        CHECK(n[1].sound.sync_mode == SyncMode::SliceStart && Near(n[1].sound.sync_offset_ms, 30.0));
    }

    // Layered: 3 piles x 2 layers (tracks 3 and 4, layer 2 starts 20 ms later, snaps 50 / 10 ms):
    // Random of 3 Blends, layer 2 at its offset (its snap 40 ms before the top's), kept on role events.
    {
        std::vector<SpreadSource> items;
        for (int k = 0; k < 3; ++k) {
            const double base = 100.0 + 2.0 * k;
            items.push_back(Src(k == 0 ? "a2" : k == 1 ? "b2" : "c2", "L2", 4, base + 0.02, 0.3, 0.01));  // given first
            items.push_back(Src(k == 0 ? "a1" : k == 1 ? "b1" : "c1", "L1", 3, base, 0.3, 0.05));
        }
        const std::vector<SoundNode> n = SpreadLayerNodes(items, "FS L", &Uid);
        CHECK(n.size() == 1 + 3 * 3);
        CHECK(ChildrenOf(n, n[0].uid) == 3);
        int blends = 0;
        for (const SoundNode& b : n) {
            if (b.type != SoundNodeType::Blend) continue;
            ++blends;
            CHECK(b.parent == n[0].uid && b.name == "L1");
            CHECK(ChildrenOf(n, b.uid) == 2);
            for (const SoundNode& c : n) {
                if (c.parent != b.uid) continue;
                CHECK(c.type == SoundNodeType::Sound && c.layer_keep_offset);
                if (c.name == "L1") CHECK(Near(c.offset_ms, 0.0) && Near(c.sound.sync_offset_ms, 50.0));
                else CHECK(Near(c.offset_ms, (0.02 + 0.01 - 0.05) * 1000.0, 1e-6) && Near(c.sound.sync_offset_ms, 10.0));
            }
        }
        CHECK(blends == 3);
        {  // 13-68 v3: keep_rate and a RAV-owned group survive the store; v2's 'lvar' flag is read and dropped
            SoundStore st;
            SoundLibrary l;
            l.uid = "l";
            std::vector<SoundNode> kn = n;
            for (SoundNode& x : kn)
                if (x.type == SoundNodeType::Sound) x.sound.keep_rate = true;
            SetLibraryContent(l, kn);
            st.libraries.push_back(l);
            SwitchGroup g;
            g.uid = "g";
            g.name = "Side";
            g.rav_owned = true;
            st.groups.push_back(g);
            SoundStore back;
            std::string text = SerializeSoundStore(st);
            CHECK(ParseSoundStore(text, &back) && back.libraries.size() == 1 && back.groups.size() == 1);
            CHECK(back.groups[0].rav_owned);
            int keep = 0;
            for (const SoundNode& x : back.libraries[0].nodes) keep += x.type == SoundNodeType::Sound && x.sound.keep_rate ? 1 : 0;
            CHECK(keep == 6);
        }
        // The piles: touching items apart (cut end to end), members top first.
        std::vector<AttachItem> f;
        for (const SpreadSource& s : items) f.push_back(s.facts);
        const std::vector<std::vector<int>> piles = SpreadPiles(f);
        CHECK(piles.size() == 3 && piles[0].size() == 2 && f[static_cast<size_t>(piles[0][0])].guid == "a1");
        AttachItem x, y;
        x.pos = 0.0, x.len = 1.0, y.pos = 1.0, y.len = 1.0;
        CHECK(SpreadPiles({x, y}).size() == 2);
        CHECK(SpreadLayerNodes({}, "FS L", &Uid).empty());
    }

    // Antho 2026-10-18: a layer at playrate 1.335 (Dress Boots, snap 50 ms) syncs its copy (played at rate 1) on
    // its snap in the source, 66.7 ms in (was 50: the copies landed 16.7 ms late).
    {
        SpreadSource s = Src("d1", "Dress", 6, 300.0, 0.1667, 0.05);
        s.playrate = 1.33483985417003;
        const std::vector<SoundNode> n = SpreadLayerNodes({s}, "FS L", &Uid);
        CHECK(n.size() == 2 && Near(n[1].sound.sync_offset_ms, 50.0 * 1.33483985417003, 1e-6));
    }

    // Add to stack (Antho 2026-10-18): the pile put against the stack's event is the one nearest it (top item's snap).
    {
        std::vector<AttachItem> f(3);
        f[0].pos = 10.0, f[0].len = 0.2, f[0].snap = 0.05, f[0].track = 5;  // snap 10.05
        f[1].pos = 12.0, f[1].len = 0.2, f[1].snap = 0.05, f[1].track = 5;  // snap 12.05
        f[2].pos = 12.01, f[2].len = 0.2, f[2].snap = 0.0, f[2].track = 6;  // under it, same pile
        const std::vector<std::vector<int>> p = SpreadPiles(f);
        CHECK(p.size() == 2);
        const int k = NearestPile(f, p, 12.07);
        CHECK(k >= 0 && p[static_cast<size_t>(k)].size() == 2 && p[static_cast<size_t>(k)][0] == 1);
        CHECK(NearestPile(f, {}, 1.0) == -1);
    }

    // Antho 2026-10-18: the pile moved onto the selected marker records what it plays (its Sounds' variations,
    // the Random's child on its path), so the copies avoid repeating it.
    {
        std::vector<SpreadSource> items;
        for (int k = 0; k < 3; ++k) {
            const double base = 200.0 + 2.0 * k;
            items.push_back(Src(k == 0 ? "p1" : k == 1 ? "q1" : "r1", "L1", 3, base, 0.3, 0.05));
            items.push_back(Src(k == 0 ? "p2" : k == 1 ? "q2" : "r2", "L2", 4, base + 0.02, 0.3, 0.01));
        }
        SoundLibrary lib;
        lib.uid = "l";
        SetLibraryContent(lib, SpreadLayerNodes(items, "FS L", &Uid));
        const std::vector<SoundDraw> d = PlayedDraws(lib, {{"Q1", 2}, {"q2", 2}});  // any case
        int sounds = 0, randoms = 0;
        for (const SoundDraw& x : d) {
            const SoundNode* n = FindNode(lib, x.node);
            CHECK(n != nullptr);
            if (!n) continue;
            if (n->type == SoundNodeType::Sound) {
                ++sounds;
                CHECK(x.pick == 2 && (n->sound.source.item_guid == "q1" || n->sound.source.item_guid == "q2"));
            } else if (n->type == SoundNodeType::Random) {
                ++randoms;
                CHECK(n->uid == lib.root && x.pick == 1);  // q's pile is the second in time
            }
        }
        CHECK(sounds == 2 && randoms == 1 && d.size() == 3);
        CHECK(PlayedDraws(lib, {{"zz", 0}}).empty());
    }

    // The Attached library: found by type (attached only, not a User Library), content replaced on a re-run.
    {
        SoundStore st;
        SoundLibrary proj;
        proj.uid = "p";
        proj.name = "FS L";  // a project library of that name is not the Attached one
        st.libraries.push_back(proj);
        CHECK(FindAttachedLibrary(st, "FS L") == nullptr);
        SoundLibrary att;
        att.uid = "a";
        att.name = "FS L";
        att.attached = true;
        st.libraries.push_back(att);
        CHECK(FindAttachedLibrary(st, " FS L ") == &st.libraries[1]);
        CHECK(FindAttachedLibrary(st, "FS R") == nullptr);
        const SoundStore& cst = st;
        CHECK(FindAttachedLibrary(cst, "FS L") == &st.libraries[1]);

        SoundLibrary& lib = st.libraries[1];
        SetLibraryContent(lib, SpreadLayerNodes({Src("s1", "A", 1, 0.0, 0.2, 0.0), Src("s2", "B", 1, 1.0, 0.2, 0.0)}, "FS L", &Uid));
        CHECK(lib.nodes.size() == 3 && lib.root == lib.nodes[0].uid);
        // Re-run: same library, content replaced.
        SetLibraryContent(lib, SpreadLayerNodes({Src("t1", "C", 1, 0.0, 0.2, 0.0)}, "FS L", &Uid));
        CHECK(lib.nodes.size() == 2 && lib.root == lib.nodes[0].uid && lib.nodes[1].sound.source.item_guid == "t1");
        CHECK(st.libraries.size() == 2);

        // Add to stack: 3 variations of a new layer selected 20 ms after a marker: root Blend[old, Random(new)].
        CHECK(Near(StackLayerOffset(10.02, {5.0, 10.0, 15.0}), 0.02));
        CHECK(Near(StackLayerOffset(9.99, {5.0, 10.0, 15.0}), -0.01));
        CHECK(Near(StackLayerOffset(7.5, {5.0, 10.0}), 2.5));  // tie: the earlier marker
        CHECK(Near(StackLayerOffset(7.0, {}), 0.0));
        const std::string old_root = lib.root;
        std::vector<SoundNode> layer = SpreadLayerNodes(
            {Src("n1", "N", 2, 10.02, 0.1, 0.0), Src("n2", "N", 2, 11.0, 0.1, 0.0), Src("n3", "N", 2, 12.0, 0.1, 0.0)}, "Layer 2", &Uid);
        const std::string new_random = layer[0].uid;
        AddStackLayer(lib, layer, StackLayerOffset(10.02, {10.0, 11.0}) * 1000.0, "blend");
        CHECK(lib.root == "blend");
        const SoundNode* root = FindNode(lib, lib.root);
        CHECK(root && root->type == SoundNodeType::Blend && root->parent.empty() && root->name == "FS L");
        CHECK(ChildrenOf(lib.nodes, "blend") == 2);
        const SoundNode* o = FindNode(lib, old_root);
        const SoundNode* r = FindNode(lib, new_random);
        CHECK(o && o->parent == "blend" && Near(o->offset_ms, 0.0));
        CHECK(r && r->parent == "blend" && r->type == SoundNodeType::Random && Near(r->offset_ms, 20.0, 1e-6) && r->layer_keep_offset);
        CHECK(ChildrenOf(lib.nodes, new_random) == 3);
        // A third layer joins the root Blend (no new wrap).
        std::vector<SoundNode> third = SpreadLayerNodes({Src("z", "Z", 1, 0.0, 0.1, 0.0)}, "Layer 3", &Uid);
        AddStackLayer(lib, third, -5.0, "blend2");
        CHECK(lib.root == "blend" && ChildrenOf(lib.nodes, "blend") == 3 && !FindNode(lib, "blend2"));
        // No root: the layer becomes the root, offset 0.
        SoundLibrary empty;
        AddStackLayer(empty, SpreadLayerNodes({Src("e", "E", 1, 0.0, 0.1, 0.0)}, "FS L", &Uid), 20.0, "b");
        CHECK(!empty.root.empty() && empty.nodes[0].uid == empty.root && Near(empty.nodes[0].offset_ms, 0.0));

        // The attached flag round-trips in the store text.
        SoundStore back;
        CHECK(ParseSoundStore(SerializeSoundStore(st), &back));
        CHECK(back.libraries.size() == 2 && !back.libraries[0].attached && back.libraries[1].attached);
        CHECK(back.libraries[1].root == "blend");
    }

    // Layer names: a spread then 2 adds: "Layer 2", "Layer 3".
    {
        SoundLibrary lib;
        lib.name = "FS L";
        SetLibraryContent(lib, SpreadLayerNodes({Src("a", "A", 1, 0.0, 0.2, 0.0)}, "FS L", &Uid));
        std::vector<SoundNode> l2 = SpreadLayerNodes({Src("b", "B", 1, 0.0, 0.2, 0.0)}, "x", &Uid);
        l2[0].name = StackLayerName(lib);
        CHECK(l2[0].name == "Layer 2");
        AddStackLayer(lib, l2, 0.0, "blend");
        std::vector<SoundNode> l3 = SpreadLayerNodes({Src("c", "C", 1, 0.0, 0.2, 0.0)}, "x", &Uid);
        l3[0].name = StackLayerName(lib);
        CHECK(l3[0].name == "Layer 3");
        AddStackLayer(lib, l3, 0.0, "blend2");
        CHECK(StackLayerName(lib) == "Layer 4" && ChildrenOf(lib.nodes, "blend") == 3);
        CHECK(StackLayerName(SoundLibrary{}) == "Layer 2");
    }

    // An NVK folder pile: the pile (the folder item and its selected children) is ONE Sound, its source the
    // folder item, its children's takes its variations (shared by the layers); plain layers stay a Blend.
    {
        const SoundData d = PileSoundData("fold", 4, 0.5);
        CHECK(d.source.kind == SoundSourceKind::Item && d.source.item_guid == "fold" && d.linked_copies);
        CHECK(d.mode == DetectMode::Takes && d.frames.size() == 4 && Near(d.frames[0].length, 0.5));
        const SoundData w = PileSoundData("fold", 1, 0.5);
        CHECK(w.mode == DetectMode::Whole && w.frames.size() == 1);
        // Take markers (NVK): one variation per slice of the top member, taking over the takes.
        const SoundData tm = PileSoundData("fold", 4, 0.5, {{0.0, 0.3, "1"}, {1.0, 0.25, "2"}, {2.0, 0.4, "3"}});
        CHECK(tm.mode == DetectMode::TakeMarkers && tm.frames.size() == 3 && tm.linked_copies);
        CHECK(Near(tm.frames[1].start, 1.0) && Near(tm.frames[1].length, 0.25));
        SpreadSource folder = Src("fold", "Steps", 1, 20.0, 0.5, 0.02);
        folder.folder = true;
        folder.sound = d;
        std::vector<SpreadSource> items = {Src("kid1", "K1", 2, 20.0, 0.4, 0.0, 4), folder, Src("kid2", "K2", 3, 20.01, 0.4, 0.0, 4),
                                           Src("solo", "S", 2, 30.0, 0.4, 0.0)};
        const std::vector<SoundNode> n = SpreadLayerNodes(items, "FS L", &Uid);
        CHECK(n.size() == 3 && ChildrenOf(n, n[0].uid) == 2);
        CHECK(n[1].type == SoundNodeType::Sound && n[1].sound.source.item_guid == "fold" && n[1].sound.mode == DetectMode::Takes
              && n[1].sound.linked_copies && n[1].sound.frames.size() == 4);
        CHECK(n[2].type == SoundNodeType::Sound && n[2].sound.source.item_guid == "solo");
    }

    // The stored marker type: a renamed Attached library is still the type's; it round-trips in the store text.
    {
        SoundStore   st;
        SoundLibrary l;
        l.uid = "a";
        l.name = "Footsteps left";  // renamed by hand
        l.attached = true;
        l.attached_type = "FS L";
        st.libraries.push_back(l);
        SoundLibrary old;  // an older one, no stored type: found by its name
        old.uid = "o";
        old.name = "FS R";
        old.attached = true;
        st.libraries.push_back(old);
        CHECK(FindAttachedLibrary(st, "FS L") == &st.libraries[0]);
        CHECK(FindAttachedLibrary(st, "Footsteps left") == nullptr);
        CHECK(FindAttachedLibrary(st, "FS R") == &st.libraries[1]);
        SoundStore back;
        CHECK(ParseSoundStore(SerializeSoundStore(st), &back));
        CHECK(back.libraries.size() == 2 && back.libraries[0].attached_type == "FS L" && back.libraries[1].attached_type.empty());
        CHECK(FindAttachedLibrary(back, "FS L") && FindAttachedLibrary(back, "FS L")->uid == "a");
    }

    // The source zone: after the project end + 1 s, relative positions kept.
    {
        const std::vector<double> to = SourceZonePositions(60.0, {12.0, 10.0, 10.5});
        CHECK(to.size() == 3 && Near(to[0], 63.0) && Near(to[1], 61.0) && Near(to[2], 61.5));
        CHECK(SourceZonePositions(60.0, {}).empty());
    }

    // The report.
    {
        SpreadRunCounts c;
        c.type = "FS L";
        c.variations = 3;
        c.layers = 6;
        c.animations = 3;
        c.events = 24;
        c.placed = 24;
        CHECK(SpreadRunReport(c) == "FS L (this animation): new library FS L, 3 variations (6 items), 3 animations, 24 sounds placed");
        c.to_source = 1;
        CHECK(SpreadRunReport(c) == "FS L (this animation): new library FS L, 3 variations (6 items), 3 animations, 24 sounds placed, "
                                    "1 earlier sound to the source zone");
        c.to_source = 0;
        c.all_instances = true;
        c.reused = true;
        c.selected_markers = 2;
        c.layers = 3;
        c.replaced = 2;
        c.kept = 1;
        c.skip_note = "1 skipped: placed by RAV";
        CHECK(SpreadRunReport(c)
              == "Used FS L (2 markers selected)\nFS L (the project): library FS L, 3 variations, 3 animations, 24 sounds placed, "
                 "2 changed, 1 already in place, 1 skipped: placed by RAV");
        SpreadRunCounts a;
        a.type = "FS L";
        a.add_layer = true;
        a.variations = 1;
        a.layers = 1;
        a.animations = 1;
        CHECK(SpreadRunReport(a) == "FS L (this animation): layer added to library FS L for this animation, 1 variation, 1 animation, "
                                    "no marker to place on");
        a.all_instances = true;
        a.type = "FS";
        a.marker = "FS R";
        a.selected_markers = 3;
        CHECK(SpreadRunReport(a) == "Used FS R (3 markers selected)\nFS (the project): layer added to library FS, "
                                    "1 variation, 1 animation, no marker to place on");
        // 13-68e v5: a separated Group's side; Item scope.
        SpreadRunCounts s;
        s.type = "FS";
        s.side = 'R';
        s.reused = true;
        s.variations = 1;
        s.layers = 1;
        s.animations = 2;
        s.events = 4;
        s.placed = 3;
        CHECK(SpreadRunReport(s) == "FS R (this animation): library FS, 1 variation, 2 animations, 3 sounds placed");
        s.side = 0;
        s.item = true;
        s.animations = 1;
        s.placed = 0;
        s.events = 1;
        CHECK(SpreadRunReport(s) == "FS (this instance): on this marker only, 1 variation, 1 animation");
        s.add_layer = true;
        CHECK(SpreadRunReport(s) == "FS (this instance): layer on this instance only, 1 variation, 1 animation");
    }

    // 13-68 v3, both sides by default: the kind from the rule (an auto block's key without its side, its side-free
    // marker word), never the marker's name; a user rule is its marker type alone.
    {
        const SpreadKind l = SpreadKindOf("step:L", "FS", "FS L");
        const SpreadKind r = SpreadKindOf("step:R", "FS", "Pas droit");  // a renamed marker: still the kind
        CHECK(l.key == "auto:step" && l.key == r.key && l.name == "FS" && r.name == "FS");
        CHECK(SpreadKindOf("hand_pivot:L", "Hand Pivot", "Hand Pivot L").key == "auto:hand_pivot");
        CHECK(SpreadKindOf("heel:L", "Heel", "Heel L").key != l.key);  // separate parts: another kind
        CHECK(SpreadKindOf("jump:B", "Jump", "Jump").key == "auto:jump");
        const SpreadKind u = SpreadKindOf("", "", " Cloth L ");  // a user rule has no side: that kind alone
        CHECK(u.key == "Cloth L" && u.name == "Cloth L" && u.key != SpreadKindOf("", "", "Cloth R").key);
        CHECK(SpreadKindOf("step:L", "", "FS L").name == "FS L");  // no side-free word: the marker
        CHECK(SpreadKindOf("", "", "  ").key.empty());
        CHECK(RuleOfMarker({"Body", "FS L", "FS R"}, "FS R") == 2);
        CHECK(RuleOfMarker({"Body", "FS L", "FS R"}, "FS L - Preview") == 1);
        CHECK(RuleOfMarker({"Body"}, "FS L") == -1);
        // One side-free Attached library per kind, found by its key.
        SoundStore   st;
        SoundLibrary a;
        a.uid = "a";
        a.name = "FS";
        a.attached = true;
        a.attached_type = l.key;
        st.libraries.push_back(a);
        CHECK(FindAttachedLibrary(st, r.key) == &st.libraries[0] && FindAttachedLibrary(st, u.key) == nullptr);
    }

    // 13-68 v3, Add to stack: stack items of several markers are refused; the in-stack items never become a layer.
    {
        PlacedTag a, b;
        a.owner = "{ABC}";
        a.rid = "r1";
        a.t = 1.0;
        b = a;
        b.owner = "{abc}";
        b.t = 1.02;  // the same event (case, +-30 ms)
        CHECK(!SeveralStackEvents({}) && !SeveralStackEvents({a}) && !SeveralStackEvents({a, b}));
        PlacedTag c = a;
        c.t = 1.5;
        CHECK(SeveralStackEvents({a, b, c}));
        PlacedTag d = a;
        d.rid = "r2";  // the other side's rule at the same time
        CHECK(SeveralStackEvents({a, d}));
        PlacedTag e = a;
        e.owner = "{DEF}";  // another animation item
        CHECK(SeveralStackEvents({e, a}));

        SoundLibrary lib;
        SetLibraryContent(lib, SpreadLayerNodes({Src("{SRC1}", "A", 1, 0.0, 0.2, 0.0), Src("{src2}", "B", 1, 1.0, 0.2, 0.0)}, "FS", &Uid));
        CHECK(IsStackItem(lib, "{new}", true, false));     // placed for the library
        CHECK(IsStackItem(lib, "{fold}", false, true));    // the attached pile's folder item
        CHECK(IsStackItem(lib, "{src1}", false, false));   // the library's own source (any case)
        CHECK(IsStackItem(lib, "{SRC2}", false, false));
        CHECK(!IsStackItem(lib, "{new}", false, false));   // a new item
        CHECK(!IsStackItem(lib, "", false, false));
    }

    // 13-68 v3 review: an NVK folder pile moved onto the marker records its draw: the library Sound's source is the
    // folder item (its children are what is attached), keyed by the folder's GUID at its top child's variation.
    {
        SpreadSource folder = Src("{FOLD}", "Steps", 1, 20.0, 0.5, 0.02);
        folder.folder = true;
        folder.sound = PileSoundData("{FOLD}", 1, 0.5, {{0.0, 0.3, "1"}, {1.0, 0.3, "2"}, {2.0, 0.3, "3"}});
        SoundLibrary lib;
        SetLibraryContent(lib, SpreadLayerNodes({Src("kid1", "K1", 2, 20.0, 0.4, 0.0), folder, Src("other", "O", 2, 30.0, 0.4, 0.0)},
                                                "FS", &Uid));
        const std::vector<SoundDraw> d = PlayedDraws(lib, {{"{fold}", 1}});
        CHECK(d.size() == 2);
        for (const SoundDraw& x : d) {
            const SoundNode* n = FindNode(lib, x.node);
            CHECK(n != nullptr);
            if (n && n->type == SoundNodeType::Sound) CHECK(n->sound.source.item_guid == "{FOLD}" && x.pick == 1);
            if (n && n->type == SoundNodeType::Random) CHECK(n->uid == lib.root && x.pick == 0);
        }
        CHECK(PlayedDraws(lib, {{"kid1", 1}}).empty());  // a child's GUID is no Sound of the library
    }

    if (g_fail) {
        std::printf("sound_spread_plan_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("sound_spread_plan_test: all passed\n");
    return 0;
}
