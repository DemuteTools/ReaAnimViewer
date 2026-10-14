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
    // 13-29: Edit's Play, each part at its own gain, the piece's fades kept across the parts.
    SoundLibrary lib;
    lib.uid = "L";
    lib.root = "s";
    SoundNode sn = Node("s", "", SoundNodeType::Sound, "S");
    sn.sound = d;
    sn.sound.mode = DetectMode::Silence;
    lib.nodes.push_back(sn);
    std::vector<AuditionVoice> st = EditPlayVoices(lib, sn);
    CHECK(st.size() == 2);
    if (st.size() == 2) {
        CHECK(Near(st[0].start_s, 1.0) && Near(st[0].length_s, 0.5) && Near(st[0].gain_db, 4.0) && Near(st[0].delay_ms, 1000.0));
        CHECK(Near(st[1].start_s, 1.5) && Near(st[1].length_s, 0.5) && Near(st[1].gain_db, -3.0) && Near(st[1].delay_ms, 1500.0));
        CHECK(Near(st[0].fade_from_s, 0.0) && Near(st[0].fade_span_s, 1.0) && Near(st[1].fade_from_s, 0.5) &&
              Near(st[1].fade_span_s, 1.0));
        CHECK(Near(st[0].fade_in_s, kDefaultFadeIn) && Near(st[1].fade_out_s, kDefaultFadeOut));
    }
    // An excluded zone inside part a: silent, the Sound's cut_fade at its edges (two pieces).
    {
        SoundNode z = sn;
        z.sound.zones.push_back({1.2, 0.1, "", {}});
        const std::vector<AuditionVoice> sz = EditPlayVoices(lib, z);
        CHECK(sz.size() == 3);
        if (sz.size() == 3) {
            CHECK(Near(sz[0].start_s, 1.0) && Near(sz[0].length_s, 0.2) && Near(sz[0].gain_db, 4.0) &&
                  Near(sz[0].fade_out_s, kDefaultCutFade));
            CHECK(Near(sz[1].start_s, 1.3) && Near(sz[1].length_s, 0.2) && Near(sz[1].gain_db, 4.0) &&
                  Near(sz[1].fade_in_s, kDefaultCutFade) && Near(sz[1].fade_from_s, 0.0) && Near(sz[1].fade_span_s, 0.7));
            CHECK(Near(sz[2].start_s, 1.5) && Near(sz[2].gain_db, -3.0) && Near(sz[2].fade_from_s, 0.2));
        }
    }
    // 13-54: Silence + divided with its parts' margins stored: only what the parts play is heard, the
    // Sound's cut fade on the sides the margins cut; touching parts play on as one voice.
    {
        SoundNode t = sn;
        t.sound.frames[0].part_db.clear();
        t.sound.frames[0].part_trims = {1.0, 1.2, 1.5, 2.0};
        t.sound.frames[0].part_trim_key = PartTrimKey(t.sound, 0, FileVersion{});  // 13-54 review: as measured
        const std::vector<AuditionVoice> sm = EditPlayVoices(lib, t);
        CHECK(sm.size() == 2);
        if (sm.size() == 2) {
            CHECK(Near(sm[0].start_s, 1.0) && Near(sm[0].length_s, 0.2) && Near(sm[0].fade_in_s, kDefaultFadeIn) &&
                  Near(sm[0].fade_out_s, kDefaultCutFade) && Near(sm[0].delay_ms, 1000.0));
            CHECK(Near(sm[1].start_s, 1.5) && Near(sm[1].length_s, 0.5) && Near(sm[1].fade_in_s, kDefaultCutFade) &&
                  Near(sm[1].fade_out_s, kDefaultFadeOut) && Near(sm[1].delay_ms, 1500.0));
        }
        t.sound.frames[0].part_trims = {1.0, 1.4, 1.4, 2.0};  // a pre-roll reaching back over the cut
        const std::vector<AuditionVoice> one = EditPlayVoices(lib, t);
        CHECK(one.size() == 1 && Near(one[0].length_s, 1.0));
        // With part gains: split where the playing part changes (1.4, not the cut).
        t.sound.frames[0].part_db["a"] = 4.0;
        const std::vector<AuditionVoice> two = EditPlayVoices(lib, t);
        CHECK(two.size() == 2 && Near(two[0].length_s, 0.4) && Near(two[0].gain_db, 4.0) && Near(two[1].gain_db, -3.0));
        // The part's own audition (a click on it) plays its piece.
        t.sound.frames[0].part_trims = {1.0, 1.2, 1.5, 2.0};
        const std::vector<AuditionVoice> pa = PartAuditionVoices(lib, t, 0, "a");
        CHECK(pa.size() == 1 && Near(pa[0].length_s, 0.2));
    }
    // Without part gains: one voice for the variation, at its dB.
    sn.sound.frames[0].part_db.clear();
    st = EditPlayVoices(lib, sn);
    CHECK(st.size() == 1 && Near(st[0].length_s, 1.0) && Near(st[0].gain_db, -3.0) && Near(st[0].fade_span_s, 0.0));
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

// 13-29: the Edit window's Play plays what is shown, as placed: on variations only (trims, fades, dB, the
// root's volume), on the file's clock; off variations and the gaps silent; the earlier variation wins.
static void TestEditPlay()
{
    SoundLibrary lib;
    lib.uid = "L";
    lib.root = "r";
    SoundNode r = Node("r", "", SoundNodeType::Random, "R");
    r.volume_db = -1.0;
    lib.nodes.push_back(r);
    SoundNode n = Node("s", "r", SoundNodeType::Sound, "S");
    n.sound.mode = DetectMode::Silence;
    SoundFrame a, b, c, off;
    a.start = 1.0, a.length = 1.0, a.db = -3.0;
    a.fade_in = 0.1;
    b.start = 0.5, b.length = 0.25, b.db = 2.0;  // listed after, starts before
    c.start = 1.5, c.length = 1.0, c.db = -6.0;  // overlaps a: a wins until 2.0
    off.start = 3.0, off.length = 0.5, off.on = false;
    n.sound.frames = {a, b, c, off};
    n.sound.mono = MonoMode::Left;
    lib.nodes.push_back(n);
    const std::vector<AuditionVoice> v = EditPlayVoices(lib, n);
    CHECK(v.size() == 3);
    if (v.size() == 3) {
        CHECK(v[0].variation == 1 && Near(v[0].start_s, 0.5) && Near(v[0].length_s, 0.25) && Near(v[0].delay_ms, 500.0) &&
              Near(v[0].gain_db, 1.0));
        CHECK(v[1].variation == 0 && Near(v[1].start_s, 1.0) && Near(v[1].gain_db, -4.0) && Near(v[1].fade_in_s, 0.1) &&
              Near(v[1].fade_out_s, kDefaultFadeOut));
        CHECK(v[2].variation == 2 && Near(v[2].start_s, 2.0) && Near(v[2].length_s, 0.5) && Near(v[2].delay_ms, 2000.0) &&
              Near(v[2].fade_from_s, 0.5) && Near(v[2].fade_span_s, 1.0) && Near(v[2].gain_db, -7.0));
        for (const AuditionVoice& x : v) CHECK(x.mono == MonoMode::Left);
    }
    // Nothing on: nothing plays.
    SoundNode none = n;
    for (SoundFrame& f : none.sound.frames) f.on = false;
    CHECK(EditPlayVoices(lib, none).empty());
    // Takes mode: the take asked for, whole, at delay 0; an off take or a bad index: nothing.
    SoundNode t = n;
    t.sound.mode = DetectMode::Takes;
    const std::vector<AuditionVoice> tv = EditPlayVoices(lib, t, 2);
    CHECK(tv.size() == 1 && tv[0].variation == 2 && Near(tv[0].delay_ms, 0.0) && Near(tv[0].length_s, 1.0));
    CHECK(EditPlayVoices(lib, t, 3).empty() && EditPlayVoices(lib, t, -1).empty() && EditPlayVoices(lib, t, 9).empty());

    // Restarted at 1.25 s: the first voice over, the second cut (its fade-in already past), the third later.
    const std::vector<AuditionVoice> from = VoicesFrom(v, 1.25);
    CHECK(from.size() == 2);
    if (from.size() == 2) {
        CHECK(Near(from[0].start_s, 1.25) && Near(from[0].length_s, 0.75) && Near(from[0].delay_ms, 0.0) &&
              Near(from[0].fade_from_s, 0.25) && Near(from[0].fade_span_s, 1.0));
        CHECK(Near(from[1].delay_ms, 750.0) && Near(from[1].fade_from_s, 0.5));
        // The fades heard after the restart are the ones of the whole variation.
        CHECK(Near(FadeGainAt(from[0].fade_from_s + 0.0, from[0].fade_span_s, 0.1, 0.005), 1.0));
    }
    CHECK(VoicesFrom(v, 0.0).size() == 3 && VoicesFrom(v, 10.0).empty());
    CHECK(SameVoices(v, EditPlayVoices(lib, n)) && !SameVoices(v, from) && !SameVoices(v, {}));
    SoundNode louder = n;
    louder.sound.frames[0].db = -2.5;
    CHECK(!SameVoices(v, EditPlayVoices(lib, louder)));
    SoundNode summed = n;
    summed.sound.mono = MonoMode::Sum;
    CHECK(!SameVoices(v, EditPlayVoices(lib, summed)));
}

// 13-41: a sample forced to mono, and the channel mode a placed item gets.
static void TestMono()
{
    CHECK(Near(MonoOf(0.5, -0.25, MonoMode::Left), 0.5) && Near(MonoOf(0.5, -0.25, MonoMode::Right), -0.25));
    CHECK(Near(MonoOf(0.5, -0.25, MonoMode::Sum), 0.125) && Near(MonoOf(0.5, -0.25, MonoMode::Off), 0.5));
    SoundLibrary lib;
    lib.uid = "L";
    lib.root = "s";
    SoundNode n = Snd("s", "", "S", {0.4});
    n.sound.mono = MonoMode::Right;
    lib.nodes.push_back(n);
    CHECK(PartAuditionVoices(lib, n, 0, "")[0].mono == MonoMode::Right);
    CHECK(ZoneAuditionVoice(lib, n, 0.1, 0.1).mono == MonoMode::Right);
    SoundStore store;
    const AuditionPlan p = PlanAudition(store, lib, "s", First());
    CHECK(p.voices.size() == 1 && p.voices[0].mono == MonoMode::Right);
}

// 13-40: a child of the Mix section auditioned with its own resolution, under its parent's name.
static void TestChildAudition()
{
    SoundStore   store;
    SoundLibrary lib;
    lib.uid = "L";
    lib.root = "r";
    lib.nodes.push_back(Node("r", "", SoundNodeType::Random, "Shoes"));
    lib.nodes.push_back(Node("b", "r", SoundNodeType::Blend, "Sneakers"));
    lib.nodes.push_back(Snd("b1", "b", "Layer 1", {0.2}));
    SoundNode l2 = Snd("b2", "b", "Layer 2", {0.3});
    l2.offset_ms = 20.0;
    lib.nodes.push_back(l2);
    lib.nodes.push_back(Node("h", "r", SoundNodeType::Random, "HighHeel"));
    lib.nodes.push_back(Snd("h1", "h", "heel_a", {0.1}));
    lib.nodes.push_back(Snd("h2", "h", "heel_b", {0.1}));
    // The Blend child: both its layers, named from the child, the parent as the root.
    AuditionPlan p = PlanChildAudition(store, lib, "r", "b", First());
    CHECK(p.root == "r" && p.voices.size() == 2);
    if (p.voices.size() == 2) {
        CHECK(p.voices[0].path == (std::vector<std::string>{"b", "b1"}) && Near(p.voices[1].delay_ms, 20.0));
        CHECK(p.played == "Sneakers \xE2\x80\xBA Layer 1 + Sneakers \xE2\x80\xBA Layer 2");
    }
    std::set<std::string> lit = LitAt(p, 1.0);
    CHECK(lit.count("r") && lit.count("b") && lit.count("b1") && !lit.count("h"));
    // The Random child draws one of its own children (the picker's second).
    AuditionPicker second = First();
    second.pick_child = [](const SoundNode&, int count) { return count > 1 ? 1 : 0; };
    p = PlanChildAudition(store, lib, "r", "h", second);
    CHECK(p.voices.size() == 1 && p.voices[0].path == (std::vector<std::string>{"h", "h2"}) &&
          p.played == "HighHeel \xE2\x80\xBA heel_b");
    // A Sound child: itself.
    p = PlanChildAudition(store, lib, "h", "h1", First());
    CHECK(p.voices.size() == 1 && p.played == "heel_a" && p.root == "h");
    // Not a child of that parent / unknown: nothing.
    CHECK(PlanChildAudition(store, lib, "r", "h1", First()).voices.empty());
    CHECK(PlanChildAudition(store, lib, "r", "zz", First()).voices.empty());
}

// 13-46: the Sound panel's "Divided by" line.
static void TestDividedByText()
{
    SoundStore  store;
    SwitchGroup g;
    g.uid = "g";
    g.name = "Foot part";
    g.values = {{"vh", "Heel", {}, {}}, {"vt", "Toe", {}, {}}};
    store.groups.push_back(g);
    SoundData d;
    CHECK(DividedByText(store, d).empty());
    d.divide_group = "g";
    d.divide_values = {"vh", "vt"};
    CHECK(DividedByText(store, d) == "Divided by Foot part (Heel / Toe)");
    d.divide_values = {"vt", "gone"};
    CHECK(DividedByText(store, d) == "Divided by Foot part (Toe / ?)");
    d.divide_group = "deleted";
    CHECK(DividedByText(store, d) == "Divided by ? (? / ?)");
}

// 13-20 / 13-23: a part played alone, its pieces (the gap of an excluded zone kept), cut_fade at a cut
// or zone edge; a zone played alone; 13-31: a Blend layer with a role plays whole in an audition.
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

    // 13-31: a Blend layer with a role: an audition is a combined event (the whole variation, offset kept).
    SoundLibrary bl;
    bl.uid = "B";
    bl.root = "b";
    bl.nodes.push_back(Node("b", "", SoundNodeType::Blend, "Blend"));
    SoundNode layer = n;
    layer.parent = "b";
    layer.layer_roles = {"toe"};
    layer.offset_ms = 30.0;
    bl.nodes.push_back(layer);
    SoundStore   store;
    AuditionPlan plan = PlanAudition(store, bl, "b", First());
    const std::vector<AuditionVoice> whole = PartAuditionVoices(bl, layer, 0, "", 30.0);
    CHECK(!plan.voices.empty() && plan.voices.size() == whole.size());
    CHECK(Near(plan.voices[0].start_s, 0.0) && Near(plan.voices[0].delay_ms, 30.0));
}

// 13-49 review: a long drag restarts Play once it pauses 0.15 s, never every 0.15 s while it moves.
static void TestSettleEditPlay()
{
    auto voice = [](double db) {
        AuditionVoice v;
        v.node = "s";
        v.length_s = 1.0;
        v.gain_db = db;
        return std::vector<AuditionVoice>{v};
    };
    const std::vector<AuditionVoice> playing = voice(0.0);
    EditPlaySettle s;
    CHECK(!SettleEditPlay(s, playing, playing, 0.0));  // nothing changed
    // A drag: a new value every frame (60 fps) for 1 s: never a restart.
    bool restarted = false;
    for (int f = 1; f <= 60; ++f) restarted |= SettleEditPlay(s, voice(-0.1 * f), playing, f / 60.0);
    CHECK(!restarted);
    // It pauses: the same value 0.15 s later restarts once.
    CHECK(!SettleEditPlay(s, voice(-7.0), playing, 1.10));
    CHECK(!SettleEditPlay(s, voice(-7.0), playing, 1.20));
    CHECK(SettleEditPlay(s, voice(-7.0), playing, 1.26));
    CHECK(s.since < 0.0);
    // Undone back to what plays: nothing pending.
    CHECK(!SettleEditPlay(s, voice(-3.0), playing, 2.0));
    CHECK(!SettleEditPlay(s, playing, playing, 2.1));
    CHECK(s.since < 0.0 && !SettleEditPlay(s, playing, playing, 3.0));
}

// 13-63: REAPER's split at the edit cursor in the Sound Edit window (40757), the pure plan, then applied as
// the window does (last first, SplitFrame).
static void TestEditSplit()
{
    auto fr = [](double s, double l) {
        SoundFrame f;
        f.start = s;
        f.length = l;
        return f;
    };
    const std::vector<SoundFrame> frames = {fr(0.0, 1.0), fr(1.5, 1.0), fr(2.0, 1.0)};  // 1 and 2 overlap
    // No cursor; a cursor between variations: nothing, and why.
    EditSplitPlan p = PlanEditSplit(frames, {}, -1.0);
    CHECK(p.frames.empty() && p.why == "Click the waveform first: the edit cursor sets where to split");
    p = PlanEditSplit(frames, {}, 1.2);
    CHECK(p.frames.empty() && p.why == "The edit cursor is not inside a variation");
    // Nothing selected: every variation holding the cursor (as REAPER splits every item under it).
    p = PlanEditSplit(frames, {}, 0.4);
    CHECK(p.frames.size() == 1 && p.frames[0] == 0);
    p = PlanEditSplit(frames, {}, 2.2);
    CHECK(p.frames.size() == 2 && p.frames[0] == 1 && p.frames[1] == 2);
    // A selection: only the selected ones the cursor is in.
    p = PlanEditSplit(frames, {2}, 2.2);
    CHECK(p.frames.size() == 1 && p.frames[0] == 2);
    p = PlanEditSplit(frames, {0}, 2.2);
    CHECK(p.frames.empty() && p.why == "The edit cursor is not inside a selected variation");
    // On an edge, or a piece of 1 ms or less: whole.
    p = PlanEditSplit(frames, {}, 1.0);
    CHECK(p.frames.empty() && p.why == "The edit cursor is not inside a variation");
    p = PlanEditSplit(frames, {}, 0.0005);
    CHECK(p.frames.empty() && p.why == "Too close to the variation's edge to split");
    // Applied as the Edit window does it: last first, so each index still names its variation.
    SoundData d;
    d.frames = frames;
    p = PlanEditSplit(d.frames, {}, 2.2);
    bool changed = false;
    for (auto it = p.frames.rbegin(); it != p.frames.rend(); ++it) changed = SplitFrame(d, *it, 2.2) || changed;
    CHECK(changed && d.frames.size() == 5);
    CHECK(Near(d.frames[1].start, 1.5) && Near(d.frames[1].length, 0.7) && Near(d.frames[2].start, 2.2) &&
          Near(d.frames[2].length, 0.3));
    CHECK(Near(d.frames[3].start, 2.0) && Near(d.frames[3].length, 0.2) && Near(d.frames[4].start, 2.2) &&
          Near(d.frames[4].length, 0.8));
}

// 13-61: REAPER's item trims in the Sound Edit window (40511 / 40512 / 40508), the pure plan.
static void TestEditTrim()
{
    auto fr = [](double s, double l) {
        SoundFrame f;
        f.start = s;
        f.length = l;
        return f;
    };
    const std::vector<SoundFrame> frames = {fr(0.0, 1.0), fr(1.5, 1.0), fr(3.0, 1.0)};
    // Nothing selected: the variation under the cursor.
    EditTrimPlan p = PlanEditTrim(frames, {}, EditTrimKind::LeftOfCursor, 1.8, 0.0, 0.0);
    CHECK(p.steps.size() == 1 && p.steps[0].frame == 1 && Near(p.steps[0].start, 1.8) && Near(p.steps[0].length, 0.7));
    p = PlanEditTrim(frames, {}, EditTrimKind::RightOfCursor, 1.8, 0.0, 0.0);
    CHECK(p.steps.size() == 1 && p.steps[0].frame == 1 && Near(p.steps[0].start, 1.5) && Near(p.steps[0].length, 0.3));
    // No cursor; a cursor between variations or on an edge: nothing, and why.
    p = PlanEditTrim(frames, {}, EditTrimKind::LeftOfCursor, -1.0, 0.0, 0.0);
    CHECK(p.steps.empty() && !p.why.empty());
    p = PlanEditTrim(frames, {}, EditTrimKind::LeftOfCursor, 1.2, 0.0, 0.0);
    CHECK(p.steps.empty() && p.why == "The edit cursor is not inside a variation");
    p = PlanEditTrim(frames, {}, EditTrimKind::RightOfCursor, 1.5, 0.0, 0.0);
    CHECK(p.steps.empty());
    // A selection: only the selected variations the cursor is in.
    p = PlanEditTrim(frames, {0}, EditTrimKind::LeftOfCursor, 1.8, 0.0, 0.0);
    CHECK(p.steps.empty() && p.why == "The edit cursor is not inside a selected variation");
    p = PlanEditTrim(frames, {2, 1}, EditTrimKind::RightOfCursor, 3.25, 0.0, 0.0);
    CHECK(p.steps.size() == 1 && p.steps[0].frame == 2 && Near(p.steps[0].length, 0.25));
    // Never 1 ms or less left (no negative or empty length).
    p = PlanEditTrim(frames, {}, EditTrimKind::LeftOfCursor, 0.9995, 0.0, 0.0);
    CHECK(p.steps.empty() && p.why == "Nothing to trim there");
    p = PlanEditTrim(frames, {}, EditTrimKind::RightOfCursor, 0.0005, 0.0, 0.0);
    CHECK(p.steps.empty());
    // To the zone: nothing selected = every variation it crosses (an edge each; the one inside untouched).
    p = PlanEditTrim(frames, {}, EditTrimKind::ToZone, -1.0, 0.5, 3.0);  // 0.5 .. 3.5
    CHECK(p.steps.size() == 2);
    CHECK(p.steps[0].frame == 0 && Near(p.steps[0].start, 0.5) && Near(p.steps[0].length, 0.5));
    CHECK(p.steps[1].frame == 2 && Near(p.steps[1].start, 3.0) && Near(p.steps[1].length, 0.5));
    // Inside one variation: both its edges.
    p = PlanEditTrim(frames, {}, EditTrimKind::ToZone, 2.0, 1.6, 0.2);
    CHECK(p.steps.size() == 1 && p.steps[0].frame == 1 && Near(p.steps[0].start, 1.6) && Near(p.steps[0].length, 0.2));
    // A selection: only the selected variations the zone crosses.
    p = PlanEditTrim(frames, {2}, EditTrimKind::ToZone, -1.0, 0.5, 3.0);
    CHECK(p.steps.size() == 1 && p.steps[0].frame == 2);
    p = PlanEditTrim(frames, {1}, EditTrimKind::ToZone, -1.0, 0.2, 0.5);
    CHECK(p.steps.empty() && p.why == "The zone crosses no selected variation");
    // No zone; a zone over a whole variation (already inside it); a zone in a gap.
    p = PlanEditTrim(frames, {}, EditTrimKind::ToZone, 0.5, 0.0, 0.0);
    CHECK(p.steps.empty() && !p.why.empty());
    p = PlanEditTrim(frames, {1}, EditTrimKind::ToZone, -1.0, 1.2, 2.0);
    CHECK(p.steps.empty() && p.why == "Already inside the zone");
    p = PlanEditTrim(frames, {}, EditTrimKind::ToZone, -1.0, 1.1, 0.3);
    CHECK(p.steps.empty() && p.why == "The zone crosses no variation");
    // A selection index out of range is ignored.
    p = PlanEditTrim(frames, {7}, EditTrimKind::LeftOfCursor, 0.5, 0.0, 0.0);
    CHECK(p.steps.size() == 1 && p.steps[0].frame == 0);
    // Applied with TrimFrame (as the edge drag), a plan step lands exactly (it only shrinks).
    SoundData d;
    d.frames = frames;
    p = PlanEditTrim(d.frames, {}, EditTrimKind::ToZone, -1.0, 0.5, 3.0);
    for (const EditTrimStep& s : p.steps) TrimFrame(d, s.frame, s.start, s.length, 5.0);
    CHECK(Near(d.frames[0].start, 0.5) && Near(d.frames[0].length, 0.5) && d.frames[0].hand && d.edited);
    CHECK(Near(d.frames[1].start, 1.5) && Near(d.frames[1].length, 1.0));
    CHECK(Near(d.frames[2].start, 3.0) && Near(d.frames[2].length, 0.5));
}

// 13-62: per-part gain / mono / fades heard in Edit's Play and auditions; Mute / Solo (decision 5); Space
// (one piece) and Shift+Space (every piece of a part) of decision 13.
static void TestPartListen()
{
    PartListen l;
    CHECK(!l.Active() && PartHeard(l, "a") && PartHeard(l, ""));
    l.mute = {"b"};
    CHECK(l.Active() && PartHeard(l, "a") && !PartHeard(l, "b"));
    l.solo = {"b"};  // a solo wins over the mutes, adds to other solos
    CHECK(PartHeard(l, "b") && !PartHeard(l, "a") && !PartHeard(l, ""));

    SoundLibrary lib;
    lib.uid = "L";
    lib.root = "s";
    SoundNode sn = Node("s", "", SoundNodeType::Sound, "S");
    sn.sound.mode = DetectMode::Silence;
    sn.sound.divide_group = "g";
    sn.sound.divide_values = {"a", "b"};
    SoundFrame f;
    f.start = 1.0;
    f.length = 1.0;
    f.db = 2.0;
    f.cuts = {1.5};
    sn.sound.frames = {f};
    lib.nodes.push_back(sn);
    const std::vector<AuditionVoice> legacy = EditPlayVoices(lib, sn);
    PartListen none;
    CHECK(legacy.size() == 1 && SameVoices(legacy, EditPlayVoices(lib, sn, -1, &none)));  // no setting, no filter: as before

    // b: -3 dB and Sum of its own.
    PartSettings g;
    g.gain_db = -3.0;
    g.mono = MonoMode::Sum;
    SetPartSetting(sn.sound, "b", g);
    std::vector<AuditionVoice> v = EditPlayVoices(lib, sn);
    CHECK(v.size() == 2);
    if (v.size() == 2) {
        CHECK(Near(v[0].gain_db, 2.0) && v[0].mono == MonoMode::Off && v[0].part == "a" && Near(v[0].fade_span_s, 1.0));
        CHECK(Near(v[1].gain_db, -1.0) && v[1].mono == MonoMode::Sum && v[1].part == "b" && Near(v[1].fade_from_s, 0.5));
    }
    std::vector<AuditionVoice> pv = PartAuditionVoices(lib, sn, 0, "b");
    CHECK(pv.size() == 1 && Near(pv[0].gain_db, -1.0) && pv[0].mono == MonoMode::Sum && pv[0].part == "b");
    sn.sound.frames[0].part_db["b"] = 1.0;  // 13-28's part_db replaces the frame dB for its part: 1 - 3
    v = EditPlayVoices(lib, sn);
    CHECK(v.size() == 2 && Near(v[1].gain_db, -2.0));
    sn.sound.frames[0].part_db.clear();

    // Mute b: only a, the cut faded where b is silent.
    l = PartListen{};
    l.mute = {"b"};
    v = EditPlayVoices(lib, sn, -1, &l);
    CHECK(v.size() == 1);
    if (v.size() == 1)
        CHECK(Near(v[0].start_s, 1.0) && Near(v[0].length_s, 0.5) && Near(v[0].fade_in_s, kDefaultFadeIn) &&
              Near(v[0].fade_out_s, kDefaultCutFade) && Near(v[0].fade_span_s, 0.0) && Near(v[0].delay_ms, 1000.0));
    // Solo b: only b.
    l = PartListen{};
    l.solo = {"b"};
    v = EditPlayVoices(lib, sn, -1, &l);
    CHECK(v.size() == 1 && Near(v[0].start_s, 1.5) && Near(v[0].fade_in_s, kDefaultCutFade) &&
          Near(v[0].fade_out_s, kDefaultFadeOut));
    l.solo = {"a", "b"};  // Solo adds
    CHECK(EditPlayVoices(lib, sn, -1, &l).size() == 2);

    // b's own fade in (4 ms): its own voice, its fades on both edges (the out = the Sound's default).
    PartSettings fi;
    fi.fade_in = 0.004;
    SetPartSetting(sn.sound, "b", fi);
    v = EditPlayVoices(lib, sn);
    CHECK(v.size() == 2);
    if (v.size() == 2)
        CHECK(Near(v[1].fade_in_s, 0.004) && Near(v[1].fade_out_s, kDefaultFadeOut) && Near(v[1].fade_span_s, 0.0) &&
              Near(v[0].fade_span_s, 1.0));

    // Space: one piece (never filtered); index past the end: nothing.
    std::vector<AuditionVoice> one = PiecePlayVoices(lib, sn, 0, 0);
    CHECK(one.size() == 1 && Near(one[0].start_s, 1.0) && Near(one[0].length_s, 0.5) && Near(one[0].gain_db, 2.0) &&
          Near(one[0].fade_in_s, kDefaultFadeIn) && Near(one[0].fade_out_s, kDefaultCutFade) && one[0].part == "a");
    one = PiecePlayVoices(lib, sn, 0, 1);
    CHECK(one.size() == 1 && Near(one[0].start_s, 1.5) && Near(one[0].gain_db, -1.0) && Near(one[0].fade_in_s, 0.004) &&
          one[0].mono == MonoMode::Sum && Near(one[0].delay_ms, 0.0));
    CHECK(PiecePlayVoices(lib, sn, 0, 2).empty() && PiecePlayVoices(lib, sn, 1, 0).empty());

    // No merge in Edit (decision 10): a | b | a, b's range given to a: three pieces, Space plays the middle one
    // alone; Shift+Space plays a's stretch as placed (one merged piece, C3).
    SoundNode cm = sn;
    cm.sound.frames[0].cuts = {1.3, 1.6};
    cm.sound.frames[0].owners = {"a", "a", "a"};
    CHECK(EditPiecesOf(cm.sound, 0).size() == 3);
    one = PiecePlayVoices(lib, cm, 0, 1);
    CHECK(one.size() == 1 && Near(one[0].start_s, 1.3) && Near(one[0].length_s, 0.3));
    CHECK(PartPiecesVoices(lib, cm, "a").size() == 1);

    // Shift+Space: every piece of b in file order, on the file's clock from the first.
    SoundNode two = sn;
    SoundFrame f2 = f;
    f2.start = 3.0;
    f2.cuts = {3.25};
    two.sound.frames = {f2, f};  // listed out of file order
    std::vector<AuditionVoice> all = PartPiecesVoices(lib, two, "b");
    CHECK(all.size() == 2);
    if (all.size() == 2)
        CHECK(Near(all[0].start_s, 1.5) && Near(all[0].delay_ms, 0.0) && Near(all[1].start_s, 3.25) &&
              Near(all[1].delay_ms, 1750.0) && all[1].part == "b" && Near(all[1].gain_db, -1.0));
    CHECK(PartPiecesVoices(lib, two, "").empty());
}

int main()
{
    TestPartListen();
    TestEditTrim();
    TestEditSplit();
    TestEditPlay();
    TestSettleEditPlay();
    TestMono();
    TestChildAudition();
    TestDividedByText();
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
