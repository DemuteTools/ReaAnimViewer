// SPDX-License-Identifier: MIT
//
// Host test of src/sound_audition_plan.h (story 13-7b): the I/O matrix of the spec -- Blend
// audition, Switch preview, Random avoid, lit at t, lengths, re-detect keeping hand frames,
// Takes trim and refused edits, the variation count text.

#include "sound_audition_plan.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
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

static bool Near(double a, double b) { return std::fabs(a - b) < 1e-6; }

static SoundNode Node(const std::string& uid, const std::string& parent, SoundNodeType t, const std::string& name)
{
    SoundNode n;
    n.uid = uid;
    n.parent = parent;
    n.type = t;
    n.name = name;
    return n;
}

static SoundNode Snd(const std::string& uid, const std::string& parent, const std::string& name,
                     std::vector<double> lens, double db = 0.0)
{
    SoundNode n = Node(uid, parent, SoundNodeType::Sound, name);
    double    t = 0.0;
    for (double l : lens) {
        SoundFrame f;
        f.start = t;
        f.length = l;
        f.db = db;
        n.sound.frames.push_back(f);
        t += l + 0.5;
    }
    return n;
}

static AuditionPicker First()
{
    AuditionPicker p;
    p.pick_child = [](const SoundNode&, int) { return 0; };
    p.pick_variation = [](const SoundNode& n) {
        for (size_t i = 0; i < n.sound.frames.size(); ++i)
            if (n.sound.frames[i].on) return static_cast<int>(i);
        return -1;
    };
    return p;
}

static void TestBlend()
{
    // Blend (vol -3) > [Sound A off 0, Random R off 40 ms > {B, C}]
    SoundStore   store;
    SoundLibrary lib;
    lib.uid = "L";
    lib.root = "bl";
    SoundNode bl = Node("bl", "", SoundNodeType::Blend, "Blend");
    bl.volume_db = -3.0;
    lib.nodes.push_back(bl);
    SoundNode a = Snd("a", "bl", "A", {0.3}, -1.0);
    a.volume_db = -2.0;
    lib.nodes.push_back(a);
    SoundNode r = Node("r", "bl", SoundNodeType::Random, "R");
    r.offset_ms = 40.0;
    lib.nodes.push_back(r);
    lib.nodes.push_back(Snd("b", "r", "B", {0.2}));
    lib.nodes.push_back(Snd("c", "r", "C", {0.2}));
    lib.nodes.push_back(Snd("e", "r", "E", {}));  // no active variation: skipped when picked
    store.libraries.push_back(lib);

    const AuditionPlan plan = PlanAudition(store, store.libraries[0], "bl", First());
    CHECK(plan.voices.size() == 2);
    if (plan.voices.size() == 2) {
        CHECK(plan.voices[0].node == "a" && Near(plan.voices[0].delay_ms, 0.0));
        CHECK(Near(plan.voices[0].gain_db, -3.0 - 2.0 - 1.0));
        CHECK(plan.voices[1].node == "b" && Near(plan.voices[1].delay_ms, 40.0));
        CHECK(Near(plan.voices[1].gain_db, -3.0));
        CHECK((plan.voices[1].path == std::vector<std::string>{"r", "b"}));
    }
    CHECK(plan.played == "A + R \xE2\x80\xBA B");

    // Offsets above the auditioned node do not count; volumes do.
    const AuditionPlan sub = PlanAudition(store, store.libraries[0], "r", First());
    CHECK(sub.voices.size() == 1 && Near(sub.voices[0].delay_ms, 0.0) && Near(sub.voices[0].gain_db, -3.0));

    // The empty Sound is never drawn: the Random picks among its 2 playable children only.
    AuditionPicker p = First();
    int            seen = -1;
    p.pick_child = [&seen](const SoundNode&, int count) {
        seen = count;
        return count - 1;
    };
    const AuditionPlan last = PlanAudition(store, store.libraries[0], "bl", p);
    CHECK(seen == 2);
    CHECK(last.voices.size() == 2 && last.voices[1].node == "c");
    // A Random with an empty child among others: its length range ignores the empty one.
    const LengthRange rr = NodeLengthRange(store, store.libraries[0], "r");
    CHECK(Near(rr.min_s, 0.2) && Near(rr.max_s, 0.2) && !rr.varies());
    // The played text of a subset of voices (PlayNode keeps only the voices whose file is read).
    CHECK(PlayedText(store.libraries[0], {plan.voices[1]}) == "R \xE2\x80\xBA B");
    CHECK(PlayedText(store.libraries[0], {}) == "nothing");
}

static void TestSwitch()
{
    SoundStore  store;
    SwitchGroup g;
    g.uid = "mat";
    g.name = "Material";
    g.values = {{"wood", "Wood", {}}, {"grass", "Grass", {}}, {"stone", "Stone", {}}};
    store.groups.push_back(g);
    SoundLibrary lib;
    lib.uid = "L";
    lib.root = "sw";
    SoundNode sw = Node("sw", "", SoundNodeType::Switch, "Sw");
    sw.group_uid = "mat";
    sw.default_value = "grass";
    sw.cases = {{"grass", "g", {}}, {"wood", "w", {}}};
    lib.nodes.push_back(sw);
    lib.nodes.push_back(Snd("g", "sw", "G", {0.1}));
    lib.nodes.push_back(Snd("w", "sw", "W", {0.1}));
    store.libraries.push_back(lib);

    AuditionPlan plan = PlanAudition(store, store.libraries[0], "sw", First());
    CHECK(plan.voices.size() == 1 && plan.voices[0].node == "g");
    store.libraries[0].nodes[0].preview_value = "wood";
    plan = PlanAudition(store, store.libraries[0], "sw", First());
    CHECK(plan.voices.size() == 1 && plan.voices[0].node == "w");
    store.libraries[0].nodes[0].preview_value = "stone";  // empty value
    plan = PlanAudition(store, store.libraries[0], "sw", First());
    CHECK(plan.voices.empty() && plan.played == "nothing");
    // A stale preview value (deleted, or of the group before "Switch on"): the default plays.
    store.libraries[0].nodes[0].preview_value = "gone";
    plan = PlanAudition(store, store.libraries[0], "sw", First());
    CHECK(plan.voices.size() == 1 && plan.voices[0].node == "g");
    CHECK(SwitchPreviewChild(store, store.libraries[0], store.libraries[0].nodes[0]) == "g");
}

static void TestRandomAvoid()
{
    SoundStore   store;
    SoundLibrary lib;
    lib.uid = "L";
    lib.root = "r";
    SoundNode r = Node("r", "", SoundNodeType::Random, "R");
    r.avoid_repeat = 2;
    lib.nodes.push_back(r);
    lib.nodes.push_back(Snd("x", "r", "X", {0.1}));
    lib.nodes.push_back(Snd("y", "r", "Y", {0.1}));
    lib.nodes.push_back(Snd("z", "r", "Z", {0.1}));
    store.libraries.push_back(lib);
    for (uint64_t seed = 1; seed < 40; ++seed) {
        AuditionHistory       h(seed);
        std::set<std::string> got;
        for (int k = 0; k < 3; ++k) {
            const AuditionPlan p = PlanAudition(store, store.libraries[0], "r", h.Picker());
            CHECK(p.voices.size() == 1);
            if (!p.voices.empty()) got.insert(p.voices[0].node);
        }
        CHECK(got.size() == 3);
    }
    // avoid >= count: DrawPick still picks something.
    store.libraries[0].nodes[0].avoid_repeat = 5;
    AuditionHistory h(7);
    for (int k = 0; k < 5; ++k) CHECK(PlanAudition(store, store.libraries[0], "r", h.Picker()).voices.size() == 1);
}

static void TestLit()
{
    AuditionPlan plan;
    plan.root = "bl";
    AuditionVoice a;
    a.node = "a";
    a.length_s = 0.3;
    a.path = {"a"};
    AuditionVoice b;
    b.node = "b";
    b.delay_ms = 40.0;
    b.length_s = 0.2;
    b.path = {"r", "b"};
    plan.voices = {a, b};
    const std::set<std::string> lit = LitAt(plan, 250.0);
    CHECK((lit == std::set<std::string>{"bl", "a"}));
    CHECK(LitAt(plan, 100.0).count("b") == 1);
    CHECK(LitAt(plan, 400.0).empty());
}

static void TestLengths()
{
    SoundStore   store;
    SoundLibrary lib;
    lib.uid = "L";
    lib.root = "bl";
    lib.nodes.push_back(Node("bl", "", SoundNodeType::Blend, "Blend"));
    lib.nodes.push_back(Snd("s1", "bl", "S1", {1.0, 2.0}));
    SoundNode s2 = Snd("s2", "bl", "S2", {0.5});
    s2.offset_ms = 500.0;
    lib.nodes.push_back(s2);
    lib.nodes.push_back(Node("empty", "bl", SoundNodeType::Random, "Empty"));
    store.libraries.push_back(lib);
    const LengthRange r = NodeLengthRange(store, store.libraries[0], "bl");
    CHECK(Near(r.min_s, 1.0) && Near(r.max_s, 2.0) && r.varies());
    const LengthRange e = NodeLengthRange(store, store.libraries[0], "empty");
    CHECK(Near(e.min_s, 0.0) && Near(e.max_s, 0.0) && !e.varies());
}

static void TestRedetect()
{
    std::vector<SoundFrame> prev;
    for (int i = 0; i < 10; ++i) {
        SoundFrame f;
        f.start = i * 1.0;
        f.length = 0.5;
        prev.push_back(f);
    }
    prev[3].start = 2.9;
    prev[3].length = 0.9;  // hand-trimmed over [2.9, 3.8)
    prev[3].hand = true;
    std::vector<SoundFrame> det;
    for (int i = 0; i < 8; ++i) {
        SoundFrame f;
        f.start = i * 1.25;
        f.length = 0.4;
        det.push_back(f);
    }
    // det[2] = [2.5, 2.9) touches but does not overlap; det[3] = [3.75, 4.15) overlaps.
    const std::vector<SoundFrame> out = MergeHandFrames(det, prev);
    CHECK(out.size() == 8);
    bool kept = false;
    for (const SoundFrame& f : out) {
        if (f.hand && Near(f.start, 2.9)) kept = true;
        CHECK(!(Near(f.start, 3.75)));
    }
    CHECK(kept);
    for (size_t i = 1; i < out.size(); ++i) CHECK(out[i - 1].start <= out[i].start);

    // Takes: by index.
    std::vector<SoundFrame> takes_prev(3), takes_det(3);
    takes_prev[1].hand = true;
    takes_prev[1].length = 0.7;
    takes_det[1].length = 2.0;
    const std::vector<SoundFrame> t = MergeHandFrames(takes_det, takes_prev, true);
    CHECK(t.size() == 3 && Near(t[1].length, 0.7));
}

static void TestTakesTrim()
{
    SoundData d;
    d.mode = DetectMode::Takes;
    d.frames.resize(2);
    d.frames[0].start = 0.0;
    d.frames[0].length = 1.0;
    d.frames[1].start = 0.5;  // take 2's start in its own source: not a neighbour
    d.frames[1].length = 1.0;
    CHECK(TrimTakeFrame(d, 0, 0.0, 5.0, 1.8));
    CHECK(Near(d.frames[0].length, 1.8) && d.frames[0].hand && d.edited);
    CHECK(Near(d.frames[1].start, 0.5));
    CHECK(!TrimTakeFrame(d, 0, 2.0, 1.0, 1.8));  // nothing left
    CHECK(!TrimTakeFrame(d, 5, 0.0, 1.0, 1.8));
    CHECK(!FrameEditAllowed(DetectMode::Takes, FrameEdit::Add));
    CHECK(!FrameEditAllowed(DetectMode::Takes, FrameEdit::Split));
    CHECK(FrameEditAllowed(DetectMode::Takes, FrameEdit::Trim));
    CHECK(FrameEditAllowed(DetectMode::Silence, FrameEdit::Split));
}

static void TestCountText()
{
    std::vector<SoundFrame> f(8);
    f[2].on = false;
    CHECK(VariationCountText(10, f) == "10 \xE2\x86\x92 8 variations \xC2\xB7 1 off");
    f[2].on = true;
    CHECK(VariationCountText(8, f) == "8 variations");
    CHECK(VariationCountText(1, std::vector<SoundFrame>(1)) == "1 variation");
}

// The Peak Normalize gesture: its selection (NormalizeSelection) -> regions -> ApplyNormalize.
static void PeakNorm(std::vector<SoundFrame>& f, const PeakData& pk, double target, bool whole, const std::vector<int>& sel)
{
    SoundData d;
    d.frames = f;
    const NormScope               scope = whole ? NormScope::Whole : NormScope::Variation;
    const std::vector<NormRegion> regs = NormalizeRegions(d, scope, sel);
    std::vector<double>           adj;
    for (const NormRegion& r : regs) adj.push_back(RegionPeakAdjDb(pk, r, target));
    ApplyNormalize(f, scope, regs, adj);
}

static void TestNormalizeOnOnly()
{
    // Matrix "Normalize": the gesture's selection (NormalizeSelection) -> regions -> ApplyNormalize.
    PeakData pk;
    pk.rate = 10.0;
    pk.max_abs.assign(30, 0.5f);
    std::vector<SoundFrame> base(3);
    for (int i = 0; i < 3; ++i) {
        base[static_cast<size_t>(i)].start = i * 1.0;
        base[static_cast<size_t>(i)].length = 1.0;
    }
    base[1].on = false;
    // An off frame in the selection: only the on ones change.
    std::vector<SoundFrame> f = base;
    PeakNorm(f, pk, -1.0, false, NormalizeSelection(f, {0, 1, 2}));
    CHECK(f[0].db > 4.0 && Near(f[1].db, 0.0) && f[2].db > 4.0);
    // Only off frames selected: nothing.
    CHECK((NormalizeSelection(base, {1}) == std::vector<int>{-1}));
    f = base;
    PeakNorm(f, pk, -1.0, false, NormalizeSelection(f, {1}));
    for (const SoundFrame& x : f) CHECK(Near(x.db, 0.0) && !x.hand);
    // No selection: every on frame.
    CHECK(NormalizeSelection(base, {}).empty());
    f = base;
    PeakNorm(f, pk, -1.0, true, NormalizeSelection(f, {}));
    CHECK(f[0].db > 4.0 && Near(f[1].db, 0.0) && f[2].db > 4.0);
}

static void TestNormalizeTakes()
{
    // Ported from NormalizeTakeFrames: each take measured in its own file (adjustment = target - its peak).
    SoundData d;
    d.mode = DetectMode::Takes;
    d.frames.resize(3);
    for (SoundFrame& x : d.frames) x.length = 1.0;
    const double ninf = -std::numeric_limits<double>::infinity();
    auto         run = [&](std::vector<SoundFrame>& f, const std::vector<double>& peak, bool whole,
                   const std::vector<int>& sel) {
        SoundData dd = d;
        dd.frames = f;
        const NormScope               scope = whole ? NormScope::Whole : NormScope::Variation;
        const std::vector<NormRegion> regs = NormalizeRegions(dd, scope, sel);
        std::vector<double>           adj;
        for (const NormRegion& r : regs) {
            const double pk = peak[static_cast<size_t>(r.frame)];
            adj.push_back(std::isfinite(pk) ? -1.0 - pk : std::nan(""));
        }
        return ApplyNormalize(f, scope, regs, adj).changed;
    };
    // Per variation: each take to its own target.
    std::vector<SoundFrame> f = d.frames;
    CHECK(run(f, {-6.0, -3.0, -12.0}, false, {}));
    CHECK(Near(f[0].db, 5.0) && Near(f[1].db, 2.0) && Near(f[2].db, 11.0) && f[0].hand);
    // Whole: the loudest take's gain for all.
    f = d.frames;
    CHECK(run(f, {-6.0, -3.0, -12.0}, true, {}));
    CHECK(Near(f[0].db, 2.0) && Near(f[1].db, 2.0) && Near(f[2].db, 2.0));
    // A take whose peaks are not read is left as is (and does not set the loudest).
    f = d.frames;
    CHECK(run(f, {-6.0, ninf, -12.0}, true, {}));
    CHECK(Near(f[0].db, 5.0) && Near(f[1].db, 0.0) && !f[1].hand && Near(f[2].db, 5.0));
    // Selection: on ones only; {-1} = nothing.
    f = d.frames;
    f[2].on = false;
    CHECK(run(f, {-6.0, -3.0, -12.0}, false, {0, 2}));
    CHECK(Near(f[0].db, 5.0) && Near(f[1].db, 0.0) && Near(f[2].db, 0.0));
    CHECK(!run(f, {-6.0, -3.0, -12.0}, false, {-1}));
    // Again on the same frames: no change.
    CHECK(!run(f, {-6.0, -3.0, -12.0}, false, {0}));
    // Takes mode never offers Per part (a take plays whole).
    SoundData t = d;
    t.divide_group = "g";
    t.divide_values = {"a", "b"};
    CHECK(!NormPartsAvailable(t) && NormalizeRegions(t, NormScope::Part, {}).size() == 3);
}

// 13-28: FrameGainDb's fallback, and Edit's Play at each part's own gain.
static void TestPartGains()
{
    SoundFrame f;
    f.start = 1.0;
    f.length = 1.0;
    f.db = -3.0;
    CHECK(Near(FrameGainDb(f, ""), -3.0) && Near(FrameGainDb(f, "a"), -3.0));
    f.part_db["a"] = 4.0;
    CHECK(Near(FrameGainDb(f, "a"), 4.0) && Near(FrameGainDb(f, "b"), -3.0) && Near(FrameGainDb(f, ""), -3.0));
    SoundData d;
    d.divide_group = "g";
    d.divide_values = {"a", "b"};
    f.cuts = {1.5};
    d.frames = {f};
    const std::vector<WholeFileStretch> st = PartGainStretches(d, 3.0);
    CHECK(st.size() == 4);
    if (st.size() == 4) {
        CHECK(Near(st[0].start, 0.0) && Near(st[0].length, 1.0) && Near(st[0].db, 0.0));
        CHECK(Near(st[1].start, 1.0) && Near(st[1].length, 0.5) && Near(st[1].db, 4.0));
        CHECK(Near(st[2].start, 1.5) && Near(st[2].length, 0.5) && Near(st[2].db, -3.0));
        CHECK(Near(st[3].start, 2.0) && Near(st[3].length, 1.0));
    }
    // An excluded zone inside part a: that stretch at the frame's dB, a on both sides of it.
    {
        SoundData z = d;
        z.zones.push_back({1.2, 0.1, "", {}});
        const std::vector<WholeFileStretch> sz = PartGainStretches(z, 3.0);
        CHECK(sz.size() == 6);
        if (sz.size() == 6) {
            CHECK(Near(sz[1].start, 1.0) && Near(sz[1].length, 0.2) && Near(sz[1].db, 4.0));
            CHECK(Near(sz[2].start, 1.2) && Near(sz[2].length, 0.1) && Near(sz[2].db, -3.0));
            CHECK(Near(sz[3].start, 1.3) && Near(sz[3].length, 0.2) && Near(sz[3].db, 4.0));
            CHECK(Near(sz[4].start, 1.5) && Near(sz[4].db, -3.0));
        }
    }
    // A second divided frame: each at its own part gains.
    {
        SoundData two = d;
        SoundFrame g;
        g.start = 2.5;
        g.length = 0.4;
        g.db = 0.5;
        g.cuts = {2.7};
        g.part_db = {{"b", -6.0}};
        two.frames.push_back(g);
        const std::vector<WholeFileStretch> s2 = PartGainStretches(two, 3.0);
        CHECK(s2.size() == 7);
        if (s2.size() == 7) {
            CHECK(Near(s2[1].db, 4.0) && Near(s2[2].db, -3.0));
            CHECK(Near(s2[3].start, 2.0) && Near(s2[3].length, 0.5) && Near(s2[3].db, 0.0));
            CHECK(Near(s2[4].start, 2.5) && Near(s2[4].length, 0.2) && Near(s2[4].db, 0.5));
            CHECK(Near(s2[5].start, 2.7) && Near(s2[5].length, 0.2) && Near(s2[5].db, -6.0));
            CHECK(Near(s2[6].start, 2.9) && Near(s2[6].db, 0.0));
        }
    }
    // Without part gains: as the frames' version.
    d.frames[0].part_db.clear();
    CHECK(PartGainStretches(d, 3.0).size() == 3);
}

void TestAuditionEnded()
{
    CHECK(!AuditionEnded(0.0, 2.0, 0.0));       // just started
    CHECK(!AuditionEnded(0.0, 2.0, 3.0));       // not started yet: never "stalled"
    CHECK(!AuditionEnded(1.0, 2.0, 0.1));       // playing
    CHECK(!AuditionEnded(1.99, 2.0, 0.0));      // last block still to be heard
    CHECK(!AuditionEnded(0.0, 0.02, 0.0));      // a 20 ms variation plays to its end
    CHECK(AuditionEnded(2.5, 2.0, 0.0));        // past the end
    CHECK(AuditionEnded(1.2, 2.0, 0.6));        // stopped short of the end
    CHECK(AuditionEnded(0.0, 0.0, 0.0));        // nothing to play
}

// Antho 2026-10-12: the fades an audition plays (the same as placed) and their gain ramps.
static void TestFades()
{
    // Linear ramps: 0 at the start, 1 after the fade in, back to 0 at the end.
    CHECK(Near(FadeGainAt(0.0, 1.0, 0.1, 0.2), 0.0));
    CHECK(Near(FadeGainAt(0.05, 1.0, 0.1, 0.2), 0.5));
    CHECK(Near(FadeGainAt(0.1, 1.0, 0.1, 0.2), 1.0));
    CHECK(Near(FadeGainAt(0.5, 1.0, 0.1, 0.2), 1.0));
    CHECK(Near(FadeGainAt(0.9, 1.0, 0.1, 0.2), 0.5));
    CHECK(Near(FadeGainAt(1.0, 1.0, 0.1, 0.2), 0.0));
    CHECK(Near(FadeGainAt(-0.1, 1.0, 0.1, 0.2), 0.0));  // outside the voice: silent
    CHECK(Near(FadeGainAt(1.5, 1.0, 0.1, 0.2), 0.0));
    CHECK(Near(FadeGainAt(0.0, 1.0, 0.0, 0.0), 1.0));   // no fade: full gain from the first sample
    CHECK(Near(FadeGainAt(0.3, 0.0, 0.1, 0.1), 1.0));   // no length: untouched
    // Longer than the voice: scaled down together (0.6 + 0.6 on 0.6 s -> 0.3 + 0.3).
    CHECK(Near(FadeGainAt(0.15, 0.6, 0.6, 0.6), 0.5));
    CHECK(Near(FadeGainAt(0.3, 0.6, 0.6, 0.6), 1.0));
    // Shapes: fast start above linear, fast end below, S-curve at half = half.
    CHECK(FadeGainAt(0.025, 1.0, 0.1, 0.0, 1) > 0.25 + 1e-6);
    CHECK(FadeGainAt(0.025, 1.0, 0.1, 0.0, 2) < 0.25 - 1e-6);
    CHECK(Near(FadeGainAt(0.05, 1.0, 0.1, 0.0, 5), 0.5));
    CHECK(Near(FadeGainAt(0.05, 1.0, 0.1, 0.0, 6), 0.5));  // an unknown shape: linear
    // Monotonic across the fade in, never above 1.
    double prev = -1.0;
    bool   mono = true;
    for (int k = 0; k <= 100; ++k) {
        const double g = FadeGainAt(0.001 * k, 1.0, 0.1, 0.1, 3, 4);
        mono = mono && g >= prev - 1e-12 && g <= 1.0;
        prev = g;
    }
    CHECK(mono);

    // A voice carries its frame's fades: the default on a slice, its own where set.
    SoundLibrary lib;
    lib.uid = "L";
    lib.root = "s";
    SoundNode s = Snd("s", "", "Steps", {0.5, 0.004});
    s.sound.mode = DetectMode::Silence;
    s.sound.frames[0].fade_out = 0.05;
    lib.nodes = {s};
    SoundStore store;
    store.libraries = {lib};
    const AuditionPlan plan = PlanAudition(store, store.libraries[0], "s", First());
    CHECK(plan.voices.size() == 1 && Near(plan.voices[0].fade_in_s, 0.001) && Near(plan.voices[0].fade_out_s, 0.05));
    double in = 0, out = 0;
    int    si = -1, so = -1;
    AuditionFades(s.sound, 1, &in, &out, &si, &so);  // 4 ms: 1 + 5 ms scaled to 4 ms
    CHECK(Near(in + out, 0.004) && si == 0 && so == 0);
    AuditionFades(s.sound, 9, &in, &out, &si, &so);  // out of range: none
    CHECK(in == 0.0 && out == 0.0);
    // Whole file: only a fade set by hand (the placed copy keeps its item's own).
    s.sound.mode = DetectMode::Whole;
    AuditionFades(s.sound, 0, &in, &out, &si, &so);
    CHECK(in == 0.0 && Near(out, 0.05));
    s.sound.mode = DetectMode::Silence;  // a folder-pile source: no fades
    AuditionFades(s.sound, 0, &in, &out, &si, &so, true);
    CHECK(in == 0.0 && out == 0.0);
}

// The Edit window's Play: each variation at its own dB, the file between them at 0 dB.
static void TestWholeFileStretches()
{
    SoundFrame a, b, c;
    a.start = 1.0, a.length = 1.0, a.db = -3.0;
    b.start = 0.5, b.length = 0.25, b.db = 2.0;  // listed after, starts before
    c.start = 1.5, c.length = 1.0, c.db = -6.0;  // overlaps a: a wins until 2.0
    const std::vector<WholeFileStretch> w = WholeFileStretches({a, b, c}, 4.0);
    CHECK(w.size() == 6);
    auto is = [](const WholeFileStretch& x, double s, double l, double db) {
        return std::fabs(x.start - s) < 1e-9 && std::fabs(x.length - l) < 1e-9 && x.db == db;
    };
    CHECK(is(w[0], 0.0, 0.5, 0.0) && is(w[1], 0.5, 0.25, 2.0) && is(w[2], 0.75, 0.25, 0.0));
    CHECK(is(w[3], 1.0, 1.0, -3.0) && is(w[4], 2.0, 0.5, -6.0) && is(w[5], 2.5, 1.5, 0.0));
    // No frame: the whole file once; unknown length: to its end.
    CHECK(WholeFileStretches({}, 3.0).size() == 1 && is(WholeFileStretches({}, 3.0)[0], 0.0, 3.0, 0.0));
    CHECK(is(WholeFileStretches({a}, 0.0).back(), 2.0, 0.0, 0.0));
}

// 13-20 / 13-23: a part played alone, its pieces (the gap of an excluded zone kept), cut_fade at a cut
// or zone edge; a zone played alone; a Blend layer's own part in a Blend audition.
static void TestPartsAudition()
{
    SoundNode n = Snd("s", "", "Steps", {0.4, 0.4}, -2.0);  // frames [0, 0.4), [0.9, 1.3)
    n.sound.mode = DetectMode::Silence;
    n.sound.divide_group = "gc";
    n.sound.divide_values = {"vb", "vf"};
    n.sound.frames[0].cuts = {0.2};
    n.sound.frames[1].cuts = {1.1};
    AddZone(n.sound, 0.08, 0.04, "");  // inside Back of variation 0
    SoundLibrary lib;
    lib.uid = "L";
    lib.root = "s";
    lib.nodes.push_back(n);

    std::vector<AuditionVoice> v = PartAuditionVoices(lib, n, 0, "vb", 10.0);
    CHECK(v.size() == 2);
    CHECK(Near(v[0].start_s, 0.0) && Near(v[0].length_s, 0.08) && Near(v[0].delay_ms, 10.0) && v[0].piece == 0);
    CHECK(Near(v[1].start_s, 0.12) && Near(v[1].length_s, 0.08) && Near(v[1].delay_ms, 130.0) && v[1].piece == 1);
    CHECK(Near(v[0].fade_out_s, kDefaultCutFade) && Near(v[1].fade_in_s, kDefaultCutFade) &&
          Near(v[1].fade_out_s, kDefaultCutFade) && Near(v[0].fade_in_s, kDefaultFadeIn));
    CHECK(Near(v[0].gain_db, -2.0));
    CHECK(PlayedText(lib, v) == "Steps");  // the later piece is not named again
    v = PartAuditionVoices(lib, n, 1, "vf");
    CHECK(v.size() == 1 && Near(v[0].start_s, 1.1) && Near(v[0].length_s, 0.2) && Near(v[0].fade_in_s, kDefaultCutFade));
    CHECK(PartAuditionVoices(lib, n, 1, "").size() == 1);        // the whole variation
    CHECK(PartAuditionVoices(lib, n, 2, "vb").empty());          // a bad index
    // A zone played alone: its stretch, cut_fade at both ends, the level of its frame.
    AuditionVoice z = ZoneAuditionVoice(lib, n, 0.95, 0.1);
    CHECK(Near(z.start_s, 0.95) && Near(z.length_s, 0.1) && z.variation == 1 && Near(z.gain_db, -2.0) &&
          Near(z.fade_in_s, kDefaultCutFade) && Near(z.fade_out_s, kDefaultCutFade));
    CHECK(Near(ZoneAuditionVoice(lib, n, 0.5, 0.2).gain_db, 0.0));   // between the variations
    CHECK(Near(ZoneAuditionVoice(lib, n, 0.5, 0.0).length_s, 0.0));  // nothing
    // 13-28: a part with its own gain (part_db) plays at it, the others at the frame's dB; a zone in it too
    // (the root's gain is 0 here).
    {
        SoundNode g = n;
        g.sound.frames[0].part_db = {{"vb", 4.0}};
        g.sound.frames[0].db = 1.0;
        const std::vector<AuditionVoice> vb = PartAuditionVoices(lib, g, 0, "vb");
        CHECK(vb.size() == 2);
        for (const AuditionVoice& x : vb) CHECK(Near(x.gain_db, 4.0));
        const std::vector<AuditionVoice> vf = PartAuditionVoices(lib, g, 0, "vf");
        CHECK(vf.size() == 1 && Near(vf[0].gain_db, 1.0));
        CHECK(Near(ZoneAuditionVoice(lib, g, 0.02, 0.04).gain_db, 4.0));   // inside Back
        CHECK(Near(ZoneAuditionVoice(lib, g, 0.25, 0.05).gain_db, 1.0));   // inside Front
    }
    // Takes mode: the take whole.
    SoundNode t = n;
    t.sound.mode = DetectMode::Takes;
    v = PartAuditionVoices(lib, t, 0, "vb");
    CHECK(v.size() == 1 && Near(v[0].length_s, 0.4));

    // A Blend layer taking only its Front part (A9): the audition plays that part.
    SoundLibrary bl;
    bl.uid = "B";
    bl.root = "b";
    bl.nodes.push_back(Node("b", "", SoundNodeType::Blend, "Blend"));
    SoundNode layer = n;
    layer.parent = "b";
    layer.layer_part = "vf";
    bl.nodes.push_back(layer);
    SoundStore   store;
    AuditionPlan plan = PlanAudition(store, bl, "b", First());
    CHECK(plan.voices.size() == 1 && Near(plan.voices[0].start_s, 0.2) && Near(plan.voices[0].length_s, 0.2));
    // The history picker never draws a variation with nothing of that part.
    bl.nodes[1].sound.zones.clear();
    AddZone(bl.nodes[1].sound, 0.2, 0.2, "");  // variation 0's Front excluded
    AuditionHistory h(3);
    for (int k = 0; k < 5; ++k) {
        AuditionPlan p = PlanAudition(store, bl, "b", h.Picker());
        CHECK(p.voices.size() == 1 && p.voices[0].variation == 1);
    }
}

int main()
{
    TestWholeFileStretches();
    TestBlend();
    TestSwitch();
    TestRandomAvoid();
    TestLit();
    TestLengths();
    TestRedetect();
    TestTakesTrim();
    TestCountText();
    TestNormalizeOnOnly();
    TestNormalizeTakes();
    TestPartGains();
    TestAuditionEnded();
    TestFades();
    TestPartsAudition();
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("sound_audition_plan: all passed\n");
    return 0;
}
