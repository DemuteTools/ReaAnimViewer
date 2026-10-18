// SPDX-License-Identifier: MIT
//
// Host test of src/library_media_plan.h (story 13-7c): every row of the spec's I/O matrix, the
// store unchanged on a refusal, slice naming, the footer labels and the format strings.

#include "library_media_plan.h"

#include <cmath>
#include <cstdio>
#include <string>

#include "sound_entries.h"
#include "sound_record.h"

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++g_fail;                                                         \
        }                                                                     \
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

// Group Material {Wood, Concrete}; library L: root Switch "sw" (Wood empty, Concrete = Random
// "cr" holding Sound "x"); library M: root Random "mr" (empty).
static SoundStore Store()
{
    SoundStore s;
    SwitchGroup g;
    g.uid = "gmat";
    g.name = "Material";
    g.values = {{"vwood", "Wood", {}}, {"vconc", "Concrete", {}}};
    s.groups.push_back(g);
    SoundLibrary l;
    l.uid = "L";
    l.name = "Steps";
    l.root = "sw";
    SoundNode sw = Node("sw", "", SoundNodeType::Switch, "Steps");
    sw.group_uid = "gmat";
    sw.cases = {{"vconc", "cr", {}}};
    sw.default_value = "vconc";
    l.nodes = {sw, Node("cr", "sw", SoundNodeType::Random, "R"), Node("x", "cr", SoundNodeType::Sound, "X")};
    s.libraries.push_back(l);
    SoundLibrary m;
    m.uid = "M";
    m.name = "Other";
    m.root = "mr";
    m.nodes = {Node("mr", "", SoundNodeType::Random, "Other")};
    s.libraries.push_back(m);
    return s;
}

static MediaPick Whole(const std::string& path, double len, int frames)
{
    MediaPick p;
    p.path = path;
    p.size = 1000;
    p.length = len;
    p.mtime = 42;
    for (int i = 0; i < frames; ++i) {
        SoundFrame f;
        f.start = i * 0.5;
        f.length = 0.3;
        p.frames.push_back(f);
    }
    return p;
}

static MediaPick SlicePick(const std::string& path, int index)
{
    MediaPick p = Whole(path, 2.0, 0);
    p.slice = true;
    p.index = index;
    p.frame.start = index * 0.5;
    p.frame.length = 0.3;
    return p;
}

static std::string Case(const SoundStore& s, const char* lib, const char* sw, const char* value)
{
    for (const SwitchCase& c : FindNode(*FindLibrary(s, lib), sw)->cases)
        if (c.value_uid == value) return c.child_uid;
    return {};
}

// 13-67c: 2 s of peaks (1000 / s): loud hits (-6 dB) at 0.1 and 1.1 s, a quiet one (-52 dB) at 0.6 s, silence else.
// -60 dB finds 3 variations, -45 dB 2.
static PeakData Hits()
{
    PeakData p;
    p.rate = 1000.0;
    p.max_abs.assign(2000, 0.0f);
    for (int i = 100; i < 300; ++i) p.max_abs[i] = p.max_abs[i + 1000] = 0.5f;
    for (int i = 600; i < 800; ++i) p.max_abs[i] = 0.0025f;
    return p;
}

// The zones of every variation, as Edit draws them (part, edges).
static bool SameZones(const SoundData& x, const SoundData& y)
{
    if (x.frames.size() != y.frames.size()) return false;
    for (size_t i = 0; i < x.frames.size(); ++i) {
        const SoundFrame& a = x.frames[i];
        const SoundFrame& b = y.frames[i];
        if (a.start != b.start || a.length != b.length || a.on != b.on || a.zones.size() != b.zones.size()) return false;
        for (size_t k = 0; k < a.zones.size(); ++k)
            if (a.zones[k].part != b.zones[k].part || std::fabs(a.zones[k].a - b.zones[k].a) > 1e-9 ||
                std::fabs(a.zones[k].b - b.zones[k].b) > 1e-9)
                return false;
    }
    return true;
}

// What touching Threshold at its own value does (library_node_ui Redetect, no hand frame): detected again with the
// Sound's own parameters, laid out on the peaks.
static SoundData Retouched(const SoundData& d, const PeakData& pk)
{
    SoundData e = d;
    e.frames = SplitOnSilence(pk, e.silence, 2.0);
    RefreshParts(e, &pk);
    return e;
}

int main()
{
    {  // empty search: every entry of the selected database, in order, pointing into the index
        std::vector<DbFile> files(5);
        for (int i = 0; i < 5; ++i) {
            files[i].path = "f" + std::to_string(i);
            files[i].db = (i % 2) ? "B" : "A";
        }
        const std::vector<const DbFile*> all = MediaListAll(files, "");
        CHECK(all.size() == 5 && all[0] == &files[0] && all[4] == &files[4]);
        const std::vector<const DbFile*> b = MediaListAll(files, "B");
        CHECK(b.size() == 2 && b[0] == &files[1] && b[1] == &files[3]);
        CHECK(MediaListAll(files, "C").empty());
        CHECK(MediaListAll({}, "").empty());
    }
    // 13-67c: a fresh Sound from a Media Explorer use: its default stored, zoned at creation on its peaks, so the
    // main window (frames) and Edit (zones) show the same variations and touching Threshold changes nothing.
    {
        const PeakData pk = Hits();
        SoundStore s = Store();
        const LibraryRow* r = nullptr;  // inside the root Random
        MediaPick pick = Whole("C:\\Sfx\\hits.wav", 2.0, 0);
        pick.frames = SplitOnSilence(pk, SilenceParams{}, 2.0);  // the preview's detection
        CHECK(pick.frames.size() == 3);
        std::vector<std::string> created;
        std::string toast, why;
        auto peaks = [&pk](const std::string& path, PeakData* out) {
            if (path != "C:\\Sfx\\hits.wav") return false;
            *out = pk;
            return true;
        };
        CHECK(UseMediaAt(s, "M", r, {pick}, &created, &toast, &why, false, nullptr, peaks));
        CHECK(created.size() == 1);
        const SoundNode* n = created.size() == 1 ? FindNode(*FindLibrary(s, "M"), created[0]) : nullptr;
        CHECK(n && n->sound.silence.threshold_db == -60.0 && n->sound.frames.size() == 3);
        bool zoned = n != nullptr;
        if (n)
            for (const SoundFrame& f : n->sound.frames) zoned = zoned && (!f.on || !f.zones.empty());
        CHECK(zoned);
        CHECK(n && SameZones(n->sound, Retouched(n->sound, pk)));
        // Saved and read back: the threshold stays the Sound's own.
        SoundStore back;
        CHECK(ParseSoundStore(SerializeSoundStore(s), &back));
        const SoundNode* b = n ? FindNode(*FindLibrary(back, "M"), n->uid) : nullptr;
        CHECK(b && b->sound.silence.threshold_db == -60.0 && b->sound.frames.size() == 3);
        // Without peaks (not read yet): zoned all the same (proposed, as a loaded Sound).
        SoundStore s2 = Store();
        std::vector<std::string> c2;
        CHECK(UseMediaAt(s2, "M", r, {pick}, &c2, &toast, &why));
        const SoundNode* n2 = c2.size() == 1 ? FindNode(*FindLibrary(s2, "M"), c2[0]) : nullptr;
        CHECK(n2 && n2->sound.frames.size() == 3 && !n2->sound.frames[0].zones.empty());
    }
    // 13-67c: an old Sound without 'thr' reads -45 dB, and every path detects with that, never -60.
    {
        const PeakData pk = Hits();
        SoundStore     old;
        CHECK(ParseSoundStore("RAVSOUNDS 1\nlibrary uid=l name=L root=n\nnode uid=n type=sound\nsound kind=item guid=g\n", &old));
        const SoundNode* n = old.libraries.empty() ? nullptr : FindNode(old.libraries[0], "n");
        CHECK(n && n->sound.silence.threshold_db == -45.0);
        if (n) {
            SoundData d = n->sound;
            d.mode = DetectMode::Silence;
            const SoundData main = Retouched(d, pk);  // the main window's detection (its own Threshold)
            CHECK(main.frames.size() == 2);
            CHECK(SameZones(main, Retouched(main, pk)));  // Edit opened, the fader touched at -45: the same
        }
    }
    // Whole file on an empty value: one Sound "stem", Silence, 4 frames, File identity.
    {
        SoundStore s = Store();
        LibraryRow r{"sw", "vwood"};
        std::vector<std::string> created;
        std::string toast, why;
        CHECK(UseMediaAt(s, "L", &r, {Whole("C:\\Sfx\\steps_wood.wav", 2.5, 4)}, &created, &toast, &why));
        CHECK(created.size() == 1);
        const SoundNode* n = FindNode(*FindLibrary(s, "L"), Case(s, "L", "sw", "vwood"));
        CHECK(n && n->type == SoundNodeType::Sound && n->name == "steps_wood");
        CHECK(n && n->sound.mode == DetectMode::Silence && n->sound.frames.size() == 4);
        CHECK(n && n->sound.source.kind == SoundSourceKind::File && n->sound.source.asset.name == "steps_wood.wav");
        CHECK(n && n->sound.source.asset.size == 1000 && n->sound.source.asset.length == 2.5);
        CHECK(n && n->sound.source.asset.last_path == "C:\\Sfx\\steps_wood.wav" && n->sound.source.asset.mtime == 42);
        CHECK(toast == "Wood plays steps_wood.wav");
    }
    // Slices on an empty value: a Random of "stem 1/2/3", each one hand frame.
    {
        SoundStore s = Store();
        LibraryRow r{"sw", "vwood"};
        std::vector<std::string> created;
        std::string toast;
        CHECK(UseMediaAt(s, "L", &r, {SlicePick("/a/hit.wav", 0), SlicePick("/a/hit.wav", 1), SlicePick("/a/hit.wav", 2)}, &created,
                         &toast, nullptr));
        const SoundLibrary& lib = *FindLibrary(s, "L");
        const SoundNode* h = FindNode(lib, Case(s, "L", "sw", "vwood"));
        CHECK(h && h->type == SoundNodeType::Random);
        std::vector<const SoundNode*> kids = ChildrenOf(lib, h ? h->uid : "");
        CHECK(kids.size() == 3 && created.size() == 3);
        if (kids.size() == 3) {
            CHECK(kids[0]->name == "hit 1" && kids[1]->name == "hit 2" && kids[2]->name == "hit 3");
            for (const SoundNode* k : kids) {
                CHECK(k->sound.mode == DetectMode::Silence && k->sound.frames.size() == 1);
                CHECK(k->sound.frames.size() == 1 && k->sound.frames[0].hand && k->sound.edited);
            }
            CHECK(kids[1]->sound.frames[0].start == 0.5);
        }
        CHECK(toast == "Wood is now a Random of 3 sounds");
    }
    // Onto a Sound: its slot becomes a Random [X, a, b].
    {
        SoundStore s = Store();
        LibraryRow r{"cr", "x"};
        std::vector<std::string> created;
        std::string toast;
        CHECK(UseMediaAt(s, "L", &r, {Whole("/a/a.wav", 1, 0), Whole("/a/b.wav", 1, 0)}, &created, &toast, nullptr));
        const SoundLibrary& lib = *FindLibrary(s, "L");
        const SoundNode* x = FindNode(lib, "x");
        CHECK(x && x->parent != "cr");
        const SoundNode* w = x ? FindNode(lib, x->parent) : nullptr;
        CHECK(w && w->type == SoundNodeType::Random && w->parent == "cr");
        std::vector<const SoundNode*> kids = ChildrenOf(lib, w ? w->uid : "");
        CHECK(kids.size() == 3);
        if (kids.size() == 3) CHECK(kids[0]->uid == "x" && kids[1]->name == "a" && kids[2]->name == "b");
        CHECK(created.size() == 2);
        CHECK(toast == "X is now a Random of 3 sounds (the old one + 2 new)");
    }
    // Into a Random: appended.
    {
        SoundStore s = Store();
        LibraryRow r{"sw", "vconc"};  // the value row playing Random "cr"
        std::string toast;
        CHECK(UseMediaAt(s, "L", &r, {Whole("/a/a.wav", 1, 0), Whole("/a/b.wav", 1, 0)}, nullptr, &toast, nullptr));
        std::vector<const SoundNode*> kids = ChildrenOf(*FindLibrary(s, "L"), "cr");
        CHECK(kids.size() == 3);
        if (kids.size() == 3) CHECK(kids[0]->uid == "x" && kids[1]->name == "a" && kids[2]->name == "b");
        CHECK(toast == "2 sounds added to the Random");
        CHECK(MediaUseAddsInside(Store(), "L", &r));
    }
    // A Switch node row (and no row on a Switch root): refused, store unchanged.
    {
        SoundStore s = Store();
        std::string before = SerializeSoundStore(s);
        LibraryRow r{"", "sw"};
        std::string why;
        CHECK(!UseMediaAt(s, "L", &r, {Whole("/a/a.wav", 1, 0)}, nullptr, nullptr, &why));
        CHECK(why == "Pick a value row of this Switch first");
        CHECK(!UseMediaAt(s, "L", nullptr, {Whole("/a/a.wav", 1, 0)}, nullptr, nullptr, &why));
        CHECK(SerializeSoundStore(s) == before);
        std::string label = "x";
        CHECK(!MediaUseLabel(s, "L", &r, 1, false, &label) && label.empty());
    }
    // No row: inside the root Random; no library: refused.
    {
        SoundStore s = Store();
        std::string why;
        CHECK(UseMediaAt(s, "M", nullptr, {Whole("/a/a.wav", 1, 0)}, nullptr, nullptr, &why));
        CHECK(ChildrenOf(*FindLibrary(s, "M"), "mr").size() == 1);
        std::string before = SerializeSoundStore(s);
        CHECK(!UseMediaAt(s, "nolib", nullptr, {Whole("/a/a.wav", 1, 0)}, nullptr, nullptr, &why));
        CHECK(why == "Create a library first");
        CHECK(SerializeSoundStore(s) == before);
    }
    // Peaks not ready: Whole with [0, length]; length unknown: no frame.
    {
        NewNodeSpec sp = SoundSpecFor(Whole("/a/a.wav", 2.5, 0));
        CHECK(sp.sound.mode == DetectMode::Whole && sp.sound.frames.size() == 1);
        if (sp.sound.frames.size() == 1) CHECK(sp.sound.frames[0].start == 0.0 && sp.sound.frames[0].length == 2.5);
        CHECK(SoundSpecFor(Whole("/a/a.wav", -1.0, 0)).sound.frames.empty());
        CHECK(MediaFileStem("C:/x/y.z/foo.bar.wav") == "foo.bar" && MediaFileName("foo.wav") == "foo.wav");
    }
    // Labels.
    {
        SoundStore s = Store();
        std::string label;
        LibraryRow wood{"sw", "vwood"}, rr{"sw", "vconc"};
        CHECK(MediaUseLabel(s, "L", &wood, 1, false, &label) && label == "Use for Wood");
        CHECK(MediaUseLabel(s, "L", &wood, 3, true, &label) && label == "Use for Wood (3 variations)");
        CHECK(MediaUseLabel(s, "L", &rr, 2, false, &label) && label == "Add to Random (2 files)");
        CHECK(MediaUseLabel(s, "L", &rr, 1, true, &label) && label == "Add to Random (1 variation)");
        CHECK(MediaUseLabel(s, "M", nullptr, 1, false, &label) && label == "Add to Random");
        CHECK(!MediaUseLabel(s, "nolib", nullptr, 1, false, &label));
        CHECK(!MediaUseAddsInside(s, "L", &wood));
    }
    // Formats.
    {
        DbFile f;
        CHECK(FormatMediaFormat(f).empty());
        f.channels = 2;
        f.bits = 24;
        f.rate = 96000;
        CHECK(FormatMediaFormat(f) == "2ch \xC2\xB7 24b \xC2\xB7 96k");
        f.bits = -1;
        f.rate = 44100;
        CHECK(FormatMediaFormat(f) == "2ch \xC2\xB7 44.1k");
        CHECK(FormatMediaLength(-1).empty());
        CHECK(FormatMediaLength(2.5) == "0:02.5");
        CHECK(FormatMediaLength(65.04) == "1:05.0");
        CHECK(FormatMediaLength(59.97) == "1:00.0");
        std::vector<SoundFrame> fr(2);
        fr[0].start = 0.0;
        fr[0].length = 0.5;
        fr[1].start = 1.0;
        fr[1].length = 0.5;
        CHECK(FrameAt(fr, 0.2) == 0 && FrameAt(fr, 0.7) == -1 && FrameAt(fr, 1.4) == 1 && FrameAt(fr, 1.5) == -1);
    }
    // Review 13-7c: a value row playing a Sound, a value row playing a nested Switch, a Blend.
    // Library B: root Switch "bsw" (Wood = Sound "bs", Concrete = Switch "isw"); library N: root Blend "bl".
    {
        SoundStore s = Store();
        SoundLibrary b;
        b.uid = "B";
        b.name = "Nested";
        b.root = "bsw";
        SoundNode bsw = Node("bsw", "", SoundNodeType::Switch, "Nested");
        bsw.group_uid = "gmat";
        bsw.cases = {{"vwood", "bs", {}}, {"vconc", "isw", {}}};
        SoundNode isw = Node("isw", "bsw", SoundNodeType::Switch, "Inner");
        isw.group_uid = "gmat";
        b.nodes = {bsw, Node("bs", "bsw", SoundNodeType::Sound, "Snd"), isw};
        s.libraries.push_back(b);
        SoundLibrary n;
        n.uid = "N";
        n.name = "Layers";
        n.root = "bl";
        n.nodes = {Node("bl", "", SoundNodeType::Blend, "Layers")};
        s.libraries.push_back(n);

        // Value row playing a Sound: the value's case becomes a Random [old, new].
        {
            SoundStore t = s;
            LibraryRow r{"bsw", "vwood"};
            std::vector<std::string> created;
            std::string toast;
            CHECK(UseMediaAt(t, "B", &r, {Whole("/a/a.wav", 1, 0)}, &created, &toast, nullptr));
            const SoundLibrary& lib = *FindLibrary(t, "B");
            const SoundNode* w = FindNode(lib, Case(t, "B", "bsw", "vwood"));
            CHECK(w && w->type == SoundNodeType::Random);
            std::vector<const SoundNode*> kids = ChildrenOf(lib, w ? w->uid : "");
            CHECK(kids.size() == 2);
            if (kids.size() == 2) CHECK(kids[0]->uid == "bs" && kids[1]->name == "a");
            CHECK(created.size() == 1 && (kids.size() != 2 || created[0] == kids[1]->uid));
            CHECK(toast == "Wood is now a Random of 2 sounds (the old one + 1 new)");
        }
        // Value row playing a nested Switch: refused like a Switch node row, store unchanged.
        {
            SoundStore t = s;
            std::string before = SerializeSoundStore(t);
            LibraryRow r{"bsw", "vconc"};
            std::string why;
            CHECK(!UseMediaAt(t, "B", &r, {Whole("/a/a.wav", 1, 0)}, nullptr, nullptr, &why));
            CHECK(why == "Pick a value row of this Switch first");
            CHECK(SerializeSoundStore(t) == before);
            CHECK(!MediaUseLabel(t, "B", &r, 1, false, nullptr));
        }
        // Into a Blend: appended inside, Blend wording.
        {
            SoundStore t = s;
            std::string toast, label;
            std::vector<std::string> created;
            CHECK(MediaUseAddsInside(t, "N", nullptr));
            CHECK(MediaUseLabel(t, "N", nullptr, 2, false, &label) && label == "Add to Blend (2 files)");
            CHECK(UseMediaAt(t, "N", nullptr, {Whole("/a/a.wav", 1, 0), Whole("/a/b.wav", 1, 0)}, &created, &toast, nullptr));
            std::vector<const SoundNode*> kids = ChildrenOf(*FindLibrary(t, "N"), "bl");
            CHECK(kids.size() == 2 && created.size() == 2);
            if (kids.size() == 2) CHECK(kids[0]->name == "a" && kids[1]->name == "b");
            CHECK(FindNode(*FindLibrary(t, "N"), "bl")->type == SoundNodeType::Blend);
            CHECK(toast == "2 sounds added to the Blend");
        }
        CHECK(FormatMediaLength(HUGE_VAL).empty());
    }
    // A file found missing before its read: dropped from the use, named in the toast.
    {
        std::vector<MediaPick> picks = {Whole("/a/a.wav", 1, 0), Whole("/a/m.wav", 1, 0), Whole("/a/m.wav", 1, 0)};
        MediaBatch b = StartMediaBatch(picks);
        CHECK(MediaBatchLeft(b) == 2);
        MediaBatchSetMissing(b, "/a/m.wav");
        CHECK(MediaBatchLeft(b) == 1 && b.read[1] == PickRead::Missing && b.read[2] == PickRead::Missing);
        MediaBatchSetRead(b, "/a/m.wav", true, {}, 2.0);  // a missing pick is not read again
        CHECK(b.read[1] == PickRead::Missing);
        std::vector<MediaPick> used = MediaBatchUsedPicks(b);
        CHECK(used.size() == 1 && used[0].path == "/a/a.wav");
        std::vector<std::string> miss = MediaBatchMissingNames(b);
        CHECK(miss.size() == 1 && miss[0] == "m.wav" && MediaBatchFailedNames(b).empty());
        CHECK(MediaMissingToastSuffix(miss) == "; missing m.wav: not added" && MediaMissingToastSuffix({}).empty());
    }
    // Several files used at once: each whole file is read, then all Sounds are made together.
    {
        std::vector<MediaPick> picks = {Whole("/a/a.wav", 1, 0), Whole("/a/b.wav", 1, 0), Whole("/a/a.wav", 1, 0),
                                        SlicePick("/a/c.wav", 1), Whole("/a/d.wav", 1, 2)};
        picks[4].frames_known = true;  // the previewed file: already read
        MediaBatch b = StartMediaBatch(picks);
        CHECK(b.read.size() == 5);
        CHECK(b.read[3] == PickRead::Ready && b.read[4] == PickRead::Ready && b.read[0] == PickRead::Reading);
        std::vector<std::string> paths = MediaBatchPathsToRead(b);
        CHECK(paths.size() == 2 && paths[0] == "/a/a.wav" && paths[1] == "/a/b.wav");
        CHECK(MediaBatchLeft(b) == 2);
        CHECK(MediaReadingLabel(2) == "Reading 2 files..." && MediaReadingLabel(1) == "Reading 1 file...");
        SoundFrame f1, f2, f3;
        f1.start = 0.0; f1.length = 0.2;
        f2.start = 0.5; f2.length = 0.2;
        f3.start = 0.9; f3.length = 0.1;
        MediaBatchSetRead(b, "/a/zzz.wav", true, {f1}, 3.0);  // not in the batch: nothing changes
        CHECK(MediaBatchLeft(b) == 2);
        MediaBatchSetRead(b, "/a/a.wav", true, {f1, f2, f3}, 3.0);
        CHECK(MediaBatchLeft(b) == 1);
        CHECK(b.picks[0].frames.size() == 3 && b.picks[2].frames.size() == 3 && b.picks[0].length == 3.0);
        CHECK(MediaBatchFailedNames(b).empty() && MediaFailedToastSuffix({}).empty());
        MediaBatchSetRead(b, "/a/b.wav", false, {f1}, 2.0);
        CHECK(MediaBatchLeft(b) == 0 && MediaBatchPathsToRead(b).empty());
        CHECK(b.read[1] == PickRead::Failed && b.picks[1].frames.empty() && b.picks[1].length == 1.0);
        std::vector<std::string> failed = MediaBatchFailedNames(b);
        CHECK(failed.size() == 1 && failed[0] == "b.wav");
        CHECK(MediaFailedToastSuffix(failed) == "; could not read b.wav: added whole");
        CHECK(MediaFailedToastSuffix({"x.wav", "y.wav"}) == "; could not read x.wav, y.wav: added whole");

        SoundStore s = Store();
        std::vector<std::string> created;
        std::string toast;
        CHECK(UseMediaAt(s, "M", nullptr, b.picks, &created, &toast, nullptr));
        CHECK(created.size() == 5);
        const SoundLibrary& lib = *FindLibrary(s, "M");
        if (created.size() == 5) {
            const SoundNode* a = FindNode(lib, created[0]);
            const SoundNode* bb = FindNode(lib, created[1]);
            const SoundNode* c = FindNode(lib, created[3]);
            const SoundNode* d = FindNode(lib, created[4]);
            CHECK(a && a->sound.mode == DetectMode::Silence && a->sound.frames.size() == 3);
            CHECK(FindNode(lib, created[2]) && FindNode(lib, created[2])->sound.frames.size() == 3);
            CHECK(bb && bb->sound.mode == DetectMode::Whole && bb->sound.frames.size() == 1);
            CHECK(c && c->sound.frames.size() == 1 && c->sound.frames[0].hand);
            CHECK(d && d->sound.mode == DetectMode::Silence && d->sound.frames.size() == 2);
        }
        // A file read with no variation found is a Whole sound, not a failure.
        MediaBatch e = StartMediaBatch({Whole("/a/q.wav", 1, 0)});
        MediaBatchSetRead(e, "/a/q.wav", true, {}, 1.5);
        CHECK(MediaBatchLeft(e) == 0 && MediaBatchFailedNames(e).empty());
        CHECK(SoundSpecFor(e.picks[0]).sound.mode == DetectMode::Whole && e.picks[0].length == 1.5);
    }
    // The rows' missing checks (decision 8): a row is never "checking", each path checked once per
    // session, only the rows on screen, one batch out at a time.
    {
        MediaExistsCache c;
        uint64_t gen = 99;
        CHECK(c.Get("/a.wav") == MediaExistsCache::State::Unknown && !c.Missing("/a.wav"));
        CHECK(c.EndFrame(64, &gen).empty() && gen == 99 && !c.BatchOut());  // nothing on screen
        // Frame 1: rows a, b, a (drawn twice: once in the batch).
        c.Want("/a.wav");
        c.Want("/b.wav");
        c.Want("/a.wav");
        std::vector<std::string> b1 = c.EndFrame(64, &gen);
        CHECK(b1.size() == 2 && b1[0] == "/a.wav" && b1[1] == "/b.wav" && c.BatchOut());
        CHECK(c.Get("/a.wav") == MediaExistsCache::State::Checking && !c.Missing("/a.wav"));
        const uint64_t g1 = gen;
        // Frame 2 while that batch is out: scrolled to c, d; nothing sent, and their wants dropped.
        c.Want("/c.wav");
        c.Want("/d.wav");
        c.Want("/a.wav");  // checking: not asked again
        CHECK(c.EndFrame(64, &gen).empty());
        c.Answer(g1, b1, {1, 0});
        CHECK(!c.BatchOut() && c.Get("/a.wav") == MediaExistsCache::State::Found && c.Missing("/b.wav"));
        // Frame 3: scrolled on to e (c, d left the screen before their turn: never checked).
        c.Want("/e.wav");
        c.Want("/a.wav");
        c.Want("/b.wav");
        std::vector<std::string> b2 = c.EndFrame(64, &gen);
        CHECK(b2.size() == 1 && b2[0] == "/e.wav");
        CHECK(c.Get("/c.wav") == MediaExistsCache::State::Unknown && c.Get("/d.wav") == MediaExistsCache::State::Unknown);
        // Known some other way meanwhile (the preview reached it): that answer wins over the batch's.
        c.Set("/e.wav", true);
        c.Answer(gen, b2, {0});
        CHECK(c.Get("/e.wav") == MediaExistsCache::State::Found && !c.BatchOut());
        // Known paths never go out again; a batch is capped and the rest waits for the next frame.
        for (int i = 0; i < 10; ++i) c.Want("/n" + std::to_string(i) + ".wav");
        c.Want("/a.wav");
        std::vector<std::string> b3 = c.EndFrame(4, &gen);
        CHECK(b3.size() == 4 && b3[0] == "/n0.wav" && b3[3] == "/n3.wav");
        CHECK(c.Get("/n4.wav") == MediaExistsCache::State::Unknown);
        const uint64_t g3 = gen;
        // Reload while it is out: everything forgotten, the old batch's answers ignored.
        c.Clear();
        CHECK(!c.BatchOut() && c.Known() == 0);
        c.Answer(g3, b3, {0, 0, 0, 0});
        CHECK(c.Known() == 0 && !c.Missing("/n0.wav"));
        c.Want("/b.wav");
        std::vector<std::string> b4 = c.EndFrame(64, &gen);
        CHECK(b4.size() == 1 && b4[0] == "/b.wav" && gen != g3);
        c.Answer(gen, b4, {1});
        CHECK(c.Get("/b.wav") == MediaExistsCache::State::Found);
        // A large list scrolled through: only the rows of the frames when no batch is out are checked.
        MediaExistsCache big;
        int sent = 0;
        uint64_t bg = 0;
        std::vector<std::string> out;
        for (int frame = 0; frame < 1000; ++frame) {  // 30 rows on screen, scrolled one screen per frame
            for (int r = 0; r < 30; ++r) big.Want("/f" + std::to_string(frame * 30 + r));
            std::vector<std::string> b = big.EndFrame(64, &bg);
            if (!b.empty()) {
                sent += static_cast<int>(b.size());
                out = b;
            }
            if (frame % 10 == 9 && big.BatchOut()) big.Answer(bg, out, std::vector<char>(out.size(), 1));
        }
        CHECK(sent == 100 * 30);  // one screen per answered batch, not 30000 queued checks
    }
    // 13-69: Alt held at the drop / "Make a Random of the selection": ONE new Random of the Sounds.
    {  // into a Random (the value row playing "cr"): one new child Random named after the stem
        SoundStore s = Store();
        LibraryRow r{"sw", "vconc"};
        std::vector<std::string> created;
        std::string toast, rnd;
        CHECK(UseMediaAt(s, "L", &r, {Whole("/a/Wood_Step_01.wav", 1, 0), Whole("/a/wood_step_02.wav", 1, 0),
                                      Whole("/a/Wood_Step_10.wav", 1, 0)},
                         &created, &toast, nullptr, true, &rnd));
        const SoundLibrary& lib = *FindLibrary(s, "L");
        std::vector<const SoundNode*> kids = ChildrenOf(lib, "cr");
        CHECK(kids.size() == 2);  // x + the new Random
        const SoundNode* nr = FindNode(lib, rnd);
        CHECK(nr && nr->type == SoundNodeType::Random && nr->parent == "cr" && nr->name == "Wood_Step");
        CHECK(kids.size() == 2 && kids[0]->uid == "x" && kids[1]->uid == rnd);
        std::vector<const SoundNode*> in = ChildrenOf(lib, rnd);
        CHECK(in.size() == 3 && created.size() == 3);
        if (in.size() == 3) CHECK(in[0]->name == "Wood_Step_01" && in[2]->name == "Wood_Step_10");
        CHECK(toast == "New Random Wood_Step of 3 sounds in the Random Concrete");
    }
    {  // into a Blend: one new child too (as one layer)
        SoundStore s = Store();
        FindLibrary(s, "M")->nodes[0].type = SoundNodeType::Blend;
        std::string rnd;
        CHECK(UseMediaAt(s, "M", nullptr, {Whole("/a/a1.wav", 1, 0), Whole("/a/b1.wav", 1, 0)}, nullptr, nullptr, nullptr,
                         true, &rnd));
        const SoundLibrary& lib = *FindLibrary(s, "M");
        CHECK(ChildrenOf(lib, "mr").size() == 1 && ChildrenOf(lib, rnd).size() == 2);
        const SoundNode* nr = FindNode(lib, rnd);
        CHECK(nr && nr->name == "Random");  // no common stem: the type's name
    }
    {  // onto a Sound: Random [X, Random [a, b]]
        SoundStore s = Store();
        LibraryRow r{"cr", "x"};
        std::string rnd, toast;
        CHECK(UseMediaAt(s, "L", &r, {Whole("/a/hit_a.wav", 1, 0), Whole("/a/hit_b.wav", 1, 0)}, nullptr, &toast, nullptr,
                         true, &rnd));
        const SoundLibrary& lib = *FindLibrary(s, "L");
        const SoundNode* x = FindNode(lib, "x");
        const SoundNode* w = x ? FindNode(lib, x->parent) : nullptr;
        CHECK(w && w->parent == "cr" && w->type == SoundNodeType::Random);
        std::vector<const SoundNode*> kids = ChildrenOf(lib, w ? w->uid : "");
        CHECK(kids.size() == 2 && kids[0]->uid == "x" && kids[1]->uid == rnd);
        CHECK(ChildrenOf(lib, rnd).size() == 2 && FindNode(lib, rnd)->name == "hit");
        CHECK(toast == "X is now a Random of the old sound + a new Random hit of 2 sounds");
    }
    {  // on an empty value: the Random the use makes anyway (not nested), renamed
        SoundStore s = Store();
        LibraryRow r{"sw", "vwood"};
        std::string rnd;
        CHECK(UseMediaAt(s, "L", &r, {Whole("/a/crack 1.wav", 1, 0), Whole("/a/crack 2.wav", 1, 0)}, nullptr, nullptr,
                         nullptr, true, &rnd));
        const SoundLibrary& lib = *FindLibrary(s, "L");
        CHECK(!rnd.empty() && Case(s, "L", "sw", "vwood") == rnd);
        CHECK(ChildrenOf(lib, rnd).size() == 2 && FindNode(lib, rnd)->name == "crack");
    }
    {  // one pick: the plain use, no Random
        SoundStore s = Store();
        std::string rnd = "x";
        CHECK(UseMediaAt(s, "M", nullptr, {Whole("/a/a.wav", 1, 0)}, nullptr, nullptr, nullptr, true, &rnd));
        CHECK(rnd.empty() && ChildrenOf(*FindLibrary(s, "M"), "mr").size() == 1);
    }
    {  // refused on a Switch, store unchanged
        SoundStore s = Store();
        const std::string before = SerializeSoundStore(s);
        LibraryRow r{"", "sw"};
        std::string why;
        CHECK(!UseMediaAt(s, "L", &r, {Whole("/a/a.wav", 1, 0), Whole("/a/b.wav", 1, 0)}, nullptr, nullptr, &why, true));
        CHECK(SerializeSoundStore(s) == before && why == "Pick a value row of this Switch first");
    }
    // The common stem.
    CHECK(MediaCommonStem({Whole("/a/FS_Gravel_01.wav", 1, 0), Whole("/b/fs_gravel_02.wav", 1, 0)}) == "FS_Gravel");
    CHECK(MediaCommonStem({Whole("/a/Impact (1).wav", 1, 0), Whole("/a/Impact (2).wav", 1, 0)}) == "Impact");
    CHECK(MediaCommonStem({Whole("/a/a.wav", 1, 0), Whole("/a/b.wav", 1, 0)}).empty());
    CHECK(MediaCommonStem({}).empty());
    // The drag tooltip.
    {
        const SoundStore s = Store();
        LibraryRow conc{"sw", "vconc"}, x{"cr", "x"}, wood{"sw", "vwood"}, swr{"", "sw"};
        CHECK(MediaDropHint(s, "L", &conc, 12, false, false) == "12 files -> added to the Random Concrete");
        CHECK(MediaDropHint(s, "L", &conc, 12, false, true) == "12 files -> new Random in the Random Concrete");
        CHECK(MediaDropHint(s, "L", &x, 2, false, false) == "2 files -> X becomes a Random: the old sound + 2 new");
        CHECK(MediaDropHint(s, "L", &x, 2, false, true) == "2 files -> X becomes a Random: the old sound + a new Random of 2");
        CHECK(MediaDropHint(s, "L", &wood, 3, true, true) == "3 variations -> a new Random for Wood");
        CHECK(MediaDropHint(s, "L", &wood, 1, false, true) == "1 file -> plays for Wood");
        CHECK(MediaDropHint(s, "L", &swr, 2, false, false).empty());
        CHECK(MediaDropHint(s, "nolib", nullptr, 2, false, false).empty());
    }
    // Shift+click ranges and the count.
    CHECK((MediaRangeRows(2, 5, 10, 100) == std::vector<size_t>{2, 3, 4, 5}));
    CHECK((MediaRangeRows(5, 2, 10, 100) == std::vector<size_t>{2, 3, 4, 5}));
    CHECK((MediaRangeRows(-1, 3, 10, 100) == std::vector<size_t>{3}));
    CHECK((MediaRangeRows(20, 3, 10, 100) == std::vector<size_t>{3}));
    CHECK((MediaRangeRows(0, 9, 10, 3) == std::vector<size_t>{7, 8, 9}));
    CHECK((MediaRangeRows(9, 0, 10, 3) == std::vector<size_t>{0, 1, 2}));
    CHECK(MediaRangeRows(0, 10, 10, 3).empty());
    CHECK(MediaSelectedLabel(12) == "12 selected" && MediaSelectedLabel(1).empty());
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("library_media_plan: all passed\n");
    return 0;
}
