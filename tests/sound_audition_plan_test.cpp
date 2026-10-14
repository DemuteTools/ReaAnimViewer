// SPDX-License-Identifier: MIT
//
// Host test of src/sound_audition_plan.h (story 13-7b): the I/O matrix of the spec -- Blend
// audition, Switch preview, Random avoid, lit at t, lengths, re-detect keeping hand frames,
// Takes trim and refused edits, the variation count text.

#include "sound_audition_plan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <string>

#include "sound_entries.h"
#include "sound_place_plan.h"  // 13-62h review: PieceApplyFor (placement / Edit parity)
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

    // b's own fade in (4 ms): its own voice, its in side the part's; its unowned out side at the variation's
    // edge keeps the frame fade (13-62 review: decision 6 per side).
    sn.sound.frames[0].fade_out = 0.05;
    PartSettings fi;
    fi.fade_in = 0.004;
    SetPartSetting(sn.sound, "b", fi);
    v = EditPlayVoices(lib, sn);
    CHECK(v.size() == 2);
    if (v.size() == 2)
        CHECK(Near(v[1].fade_in_s, 0.004) && Near(v[1].fade_out_s, 0.05) && Near(v[1].fade_span_s, 0.0) &&
              Near(v[0].fade_span_s, 1.0));
    {  // a owns only its fade out: its in side at the variation's edge keeps the frame fade in
        SoundNode ao = sn;
        PartSettings fo;
        fo.fade_out = 0.006;
        SetPartSetting(ao.sound, "a", fo);
        std::vector<AuditionVoice> w = EditPlayVoices(lib, ao);
        CHECK(w.size() == 2);
        if (w.size() == 2) CHECK(Near(w[0].fade_in_s, kDefaultFadeIn) && Near(w[0].fade_out_s, 0.006) && Near(w[0].fade_span_s, 0.0));
    }
}

// 13-62h: the Sound's trim plan is its variations' (a divided Sound trims its zones: ZoneTrimAt).
static void TestPieceTrim()
{
    SoundData d;
    SoundFrame f;
    f.start = 1.0;
    f.length = 1.0;
    f.cuts = {1.5};
    d.frames = {f};
    EditTrimPlan p = PlanEditTrim(d, {}, EditTrimKind::RightOfCursor, 1.2, 0.0, 0.0);
    CHECK(p.steps.size() == 1 && Near(p.steps[0].length, 0.2));
    p = PlanEditTrim(d, {}, EditTrimKind::LeftOfCursor, 1.2, 0.0, 0.0);
    CHECK(p.steps.size() == 1 && Near(p.steps[0].start, 1.2) && Near(p.steps[0].length, 0.8));
    p = PlanEditTrim(d, {}, EditTrimKind::RightOfCursor, 2.5, 0.0, 0.0);
    CHECK(p.Empty() && p.why == "The edit cursor is not inside a variation");
}

// 13-62F (F5): Next / Previous variation (REAPER's 40417 / 40416) at a fixed zoom.
static void TestEditNav()
{
    auto fr = [](double s, double l, bool on = true) {
        SoundFrame f;
        f.start = s;
        f.length = l;
        f.on = on;
        return f;
    };
    // Stored out of time order on purpose; variation 2 is off (still walked).
    const std::vector<SoundFrame> frames = {fr(3.0, 1.0), fr(0.0, 1.0), fr(1.5, 1.0, false)};
    // From the selection: next in time order, the cursor at its start, the view (2 s) centred on it.
    EditNavPlan p = PlanEditNav(frames, {1}, -1.0, true, 2.0, 10.0);
    CHECK(p.frame == 2 && Near(p.cursor, 1.5) && Near(p.view_start, 1.0));
    p = PlanEditNav(frames, {1, 2}, -1.0, true, 2.0, 10.0);  // the last selected one counts
    CHECK(p.frame == 0 && Near(p.cursor, 3.0) && Near(p.view_start, 2.5));
    p = PlanEditNav(frames, {0}, -1.0, false, 2.0, 10.0);
    CHECK(p.frame == 2);
    // At the ends: nothing.
    CHECK(PlanEditNav(frames, {0}, -1.0, true, 2.0, 10.0).frame == -1);
    CHECK(PlanEditNav(frames, {1}, -1.0, false, 2.0, 10.0).frame == -1);
    // The view clamped at the source's edges; the whole source shown: start 0.
    p = PlanEditNav(frames, {2}, -1.0, false, 2.0, 10.0);
    CHECK(p.frame == 1 && Near(p.view_start, 0.0));
    p = PlanEditNav(frames, {2}, -1.0, true, 2.0, 3.5);
    CHECK(p.frame == 0 && Near(p.view_start, 1.5));
    CHECK(Near(PlanEditNav(frames, {2}, -1.0, true, 0.0, 10.0).view_start, 0.0));
    // Nothing selected, as REAPER: next = the first starting after the cursor, previous = the last starting
    // before it (the one under the cursor only when the cursor is past its start).
    CHECK(PlanEditNav(frames, {}, 1.7, true, 2.0, 10.0).frame == 0);   // under variation 2: the next one starts at 3
    CHECK(PlanEditNav(frames, {}, 1.7, false, 2.0, 10.0).frame == 2);  // past its start: it counts for previous
    CHECK(PlanEditNav(frames, {}, 1.2, true, 2.0, 10.0).frame == 2);
    CHECK(PlanEditNav(frames, {}, 1.2, false, 2.0, 10.0).frame == 1);
    CHECK(PlanEditNav(frames, {}, 1.5, false, 2.0, 10.0).frame == 1);  // on its start: not before the cursor
    CHECK(PlanEditNav(frames, {}, 1.5, true, 2.0, 10.0).frame == 0);   // nor after it
    CHECK(PlanEditNav(frames, {}, 0.0, true, 2.0, 10.0).frame == 2);
    CHECK(PlanEditNav(frames, {}, -1.0, true, 2.0, 10.0).frame == 1);  // no cursor: the first / the last
    CHECK(PlanEditNav(frames, {}, -1.0, false, 2.0, 10.0).frame == 0);
    CHECK(PlanEditNav(frames, {}, 4.5, true, 2.0, 10.0).frame == -1);
    CHECK(PlanEditNav(frames, {}, 4.5, false, 2.0, 10.0).frame == 0);
    CHECK(PlanEditNav({}, {}, 0.0, true, 2.0, 10.0).frame == -1);
    CHECK(PlanEditNav(frames, {9}, 4.5, false, 2.0, 10.0).frame == 0);  // a bad index: as nothing selected
    // Takes mode (by index: each take has its own time): index order, never the starts.
    CHECK(PlanEditNav(frames, {0}, -1.0, true, 0.0, 0.0, true).frame == 1);
    CHECK(PlanEditNav(frames, {1}, -1.0, true, 0.0, 0.0, true).frame == 2);
    CHECK(PlanEditNav(frames, {1}, -1.0, false, 0.0, 0.0, true).frame == 0);
    CHECK(PlanEditNav(frames, {2}, -1.0, true, 0.0, 0.0, true).frame == -1);
    CHECK(PlanEditNav(frames, {0}, -1.0, false, 0.0, 0.0, true).frame == -1);
    CHECK(PlanEditNav(frames, {}, 3.5, true, 0.0, 0.0, true).frame == 0);  // nothing selected: first / last
    CHECK(PlanEditNav(frames, {}, 3.5, false, 0.0, 0.0, true).frame == 2);
}

// ---- 13-62h (Lot C): a zoned Sound's Edit playback --------------------------------------------------------

static EditZone ZPart(const std::string& part, double a, double b)
{
    EditZone z;
    z.part = part;
    z.a = z.slot_a = a;
    z.b = z.slot_b = b;
    z.a_hand = z.b_hand = true;
    return z;
}

static EditZone ZGap(double a, double b, double xl, double xr)
{
    EditZone g;
    g.a = g.slot_a = a;
    g.b = g.slot_b = b;
    g.xl = xl;
    g.xr = xr;
    return g;
}

// A divided Sound (Heel / Toe), one variation holding `zones`, Sound fades 5 ms.
static SoundData Zoned(std::vector<EditZone> zones)
{
    SoundData d;
    d.mode = DetectMode::Silence;
    d.divide_group = "g";
    d.divide_values = {"heel", "toe"};
    d.fade_in = d.fade_out = 0.005;
    SoundFrame f;
    f.zones = zones;
    f.start = zones.front().a;
    f.length = zones.back().b - f.start;
    d.frames.push_back(f);
    return d;
}

static void TestZonedEmptySpace()
{
    // Matrix "Empty space": Heel [0.10, 0.30], Toe [0.40, 0.60]; Edit Play from 0.10: 0.10 s of silence between.
    const SoundData d = Zoned({ZPart("heel", 0.10, 0.30), ZPart("toe", 0.40, 0.60)});
    CHECK(UsesZones(d));
    const EditPlay p = EditPlayFrom(d, 0.10, 0.60);
    CHECK(p.voices.size() == 2);
    if (p.voices.size() == 2) {
        CHECK(Near(p.voices[0].start_s, 0.10) && Near(p.voices[0].length_s, 0.20) && Near(p.voices[0].delay_ms, 0.0));
        CHECK(p.voices[0].part == "heel" && p.voices[0].variation == 0 && p.voices[0].piece == 0);
        CHECK(Near(p.voices[0].fade_in_s, 0.005) && Near(p.voices[0].fade_out_s, 0.005));
        CHECK(Near(p.voices[1].start_s, 0.40) && Near(p.voices[1].length_s, 0.20));
        CHECK(Near(p.voices[1].delay_ms, 300.0));  // 0.20 of Heel + 0.10 of silence
        CHECK(p.voices[1].part == "toe" && p.voices[1].piece == 1);
    }
    CHECK(p.map.spans.size() == 3);
    CHECK(Near(p.map.total, 0.50));
    // In the silence: one playhead walking the empty space in file time, no zone.
    std::vector<HeardHead> h = HeardHeads(p.map, 0.25);
    CHECK(h.size() == 1 && Near(h[0].file_t, 0.35) && h[0].z.frame == -1 && Near(h[0].gain, 1.0) && !h[0].incoming);
    h = HeardHeads(p.map, 0.05);
    CHECK(h.size() == 1 && Near(h[0].file_t, 0.15) && h[0].z.index == 0 && Near(h[0].gain, 1.0));
    h = HeardHeads(p.map, 0.001);  // inside Heel's fade in: its gain rises
    CHECK(h.size() == 1 && Near(h[0].gain, 0.2));
    CHECK(HeardHeads(p.map, 0.50).empty());  // the end
    // Playing from inside Heel: its voice a window of its faded stretch.
    const EditPlay q = EditPlayFrom(d, 0.20, 0.60);
    CHECK(q.voices.size() == 2 && Near(q.voices[0].start_s, 0.20) && Near(q.voices[0].fade_from_s, 0.10) &&
          Near(q.voices[0].fade_span_s, 0.20) && Near(q.voices[1].delay_ms, 200.0));
    // Silence before the first zone and after the last one counts too (the playhead walks the file).
    const EditPlay w = EditPlayFrom(d, 0.0, 1.0);
    CHECK(w.voices.size() == 2 && Near(w.voices[0].delay_ms, 100.0) && Near(w.map.total, 1.0));
    // Not zoned (undivided) or an empty range: nothing.
    SoundData u = d;
    u.divide_group.clear();
    u.divide_values.clear();
    CHECK(EditPlayFrom(u, 0.0, 1.0).voices.empty());
    CHECK(EditPlayFrom(d, 0.5, 0.5).voices.empty());
    // The library overload: the volumes from the root on top of the zone's gain, the node's uid.
    SoundLibrary lib;
    SoundNode    n = Node("s", "", SoundNodeType::Sound, "S");
    n.volume_db = -3.0;
    n.sound = d;
    n.sound.frames[0].zones[1].db = 2.0;
    lib.nodes.push_back(n);
    lib.root = "s";
    const EditPlay l = EditPlayFrom(lib, lib.nodes[0], 0.10, 0.60);
    CHECK(l.voices.size() == 2 && l.voices[0].node == "s" && Near(l.voices[0].gain_db, -3.0) && Near(l.voices[1].gain_db, -1.0));
}

static void TestZonedGapCrossfade()
{
    // Heel [0.10, 0.40] · Gap [0.40, 0.45] xl = xr = 5 ms · Heel [0.45, 0.70]: the jump crossfaded by 10 ms.
    const SoundData d = Zoned({ZPart("heel", 0.10, 0.40), ZGap(0.40, 0.45, 0.005, 0.005), ZPart("heel", 0.45, 0.70)});
    const EditPlay  p = EditPlayFrom(d, 0.10, 0.70);
    CHECK(p.voices.size() == 2);
    if (p.voices.size() != 2) return;
    const AuditionVoice &L = p.voices[0], &R = p.voices[1];
    CHECK(Near(L.start_s, 0.10) && Near(L.start_s + L.length_s, 0.405));  // into the Gap by xl
    CHECK(Near(R.start_s, 0.445));                                         // from the Gap's end minus xr
    const double left_end_ms = L.delay_ms + L.length_s * 1000.0;
    CHECK(Near(R.delay_ms, left_end_ms - 10.0));  // the right voice starts xl + xr before the left one ends
    CHECK(Near(L.fade_out_s, 0.010) && Near(R.fade_in_s, 0.010));
    CHECK(Near(p.map.total, 0.295 + 0.255));
    // The middle of the crossfade (heard 0.300): two heads, the outgoing fading, the incoming rising, ~1 together.
    const std::vector<HeardHead> h = HeardHeads(p.map, 0.300);
    CHECK(h.size() == 2);
    if (h.size() == 2) {
        CHECK(!h[0].incoming && h[1].incoming);
        CHECK(Near(h[0].file_t, 0.400) && Near(h[1].file_t, 0.450));
        CHECK(Near(h[0].gain, 0.5) && Near(h[1].gain, 0.5) && Near(h[0].gain + h[1].gain, 1.0));
        CHECK(Near(h[0].gain, ZoneEnv(d, ZoneRef{0, 0}, h[0].file_t)) && Near(h[1].gain, ZoneEnv(d, ZoneRef{0, 2}, h[1].file_t)));
    }
    // The engine's gains (FadeGainAt on the voices) sum to 1 there too.
    const double gl = FadeGainAt(0.300 - L.delay_ms / 1000.0 + L.fade_from_s, L.fade_span_s, L.fade_in_s, L.fade_out_s);
    const double gr = FadeGainAt(0.300 - R.delay_ms / 1000.0 + R.fade_from_s, R.fade_span_s, R.fade_in_s, R.fade_out_s);
    CHECK(Near(gl + gr, 1.0));
    // Outside the crossfade: one head.
    CHECK(HeardHeads(p.map, 0.2).size() == 1);
    CHECK(HeardHeads(p.map, 0.4).size() == 1 && Near(HeardHeads(p.map, 0.4)[0].file_t, 0.55));
    // Starting inside the Gap: it resumes at its exit (no silence for the jumped stretch).
    const EditPlay g = EditPlayFrom(d, 0.43, 0.70);
    CHECK(g.voices.size() == 1 && Near(g.voices[0].start_s, 0.445) && Near(g.voices[0].delay_ms, 0.0));
}

static void TestZonedListen()
{
    // Matrix row Mute / Solo: a muted part gives no voice, its time kept.
    const SoundData d = Zoned({ZPart("heel", 0.10, 0.30), ZPart("toe", 0.40, 0.60)});
    PartListen      mute;
    mute.mute.insert("heel");
    EditPlay p = EditPlayFrom(d, 0.10, 0.60, &mute);
    CHECK(p.voices.size() == 1 && p.voices[0].part == "toe" && Near(p.voices[0].delay_ms, 300.0));
    CHECK(p.map.spans.size() == 3 && !p.map.spans[0].heard && Near(p.map.total, 0.50));
    CHECK(HeardHeads(p.map, 0.05).size() == 1 && !HeardHeads(p.map, 0.05)[0].heard);  // its playhead still moves
    PartListen solo;
    solo.solo.insert("heel");
    p = EditPlayFrom(d, 0.10, 0.60, &solo);
    CHECK(p.voices.size() == 1 && p.voices[0].part == "heel" && Near(p.map.total, 0.50));
    solo.solo.insert("toe");  // Solo adds
    CHECK(EditPlayFrom(d, 0.10, 0.60, &solo).voices.size() == 2);
    // An off variation: silent, its time kept.
    SoundData off = d;
    off.frames[0].on = false;
    p = EditPlayFrom(off, 0.10, 0.60);
    CHECK(p.voices.empty() && Near(p.map.total, 0.50));
}

static void TestEditTransport()
{
    // Matrix "Time selection": ruler drag 0.2-0.5, cursor at 0.8, Space: the cursor goes to 0.2, plays to 0.5, stops.
    EditTransport t = PlanEditTransport(0.8, true, 0.2, 0.5, false, 1.0);
    CHECK(Near(t.cursor, 0.2) && Near(t.from, 0.2) && Near(t.to, 0.5) && !t.loops);
    CHECK(!EditLoopDue(t, 10.0, 0.3));
    // Loop on: repeats 0.2-0.5.
    t = PlanEditTransport(0.8, true, 0.2, 0.5, true, 1.0);
    CHECK(Near(t.from, 0.2) && Near(t.to, 0.5) && t.loops && Near(t.loop_a, 0.2) && Near(t.loop_b, 0.5));
    CHECK(!EditLoopDue(t, 0.1, 0.3) && EditLoopDue(t, 0.3, 0.3) && EditLoopDue(t, 0.29, 0.3, 0.02));
    // The cursor inside the time selection: from it, still never out of it.
    t = PlanEditTransport(0.3, true, 0.2, 0.5, true, 1.0);
    CHECK(Near(t.cursor, 0.3) && Near(t.from, 0.3) && Near(t.to, 0.5) && Near(t.loop_a, 0.2));
    t = PlanEditTransport(0.5, true, 0.2, 0.5, false, 1.0);  // at its end = outside: back to its start
    CHECK(Near(t.from, 0.2));
    // A zone's play with a time selection elsewhere: kept inside the selection.
    t = PlanEditPlay(0.6, 0.9, true, 0.2, 0.5, false, 1.0);
    CHECK(Near(t.cursor, 0.2) && Near(t.to, 0.5));
    // No time selection: from the cursor to the end; Loop = the whole file loops.
    t = PlanEditTransport(0.3, false, 0.0, 0.0, false, 1.0);
    CHECK(Near(t.from, 0.3) && Near(t.to, 1.0) && !t.loops);
    t = PlanEditTransport(0.3, false, 0.0, 0.0, true, 1.0);
    CHECK(Near(t.from, 0.3) && t.loops && Near(t.loop_a, 0.0) && Near(t.loop_b, 1.0));
    t = PlanEditTransport(-1.0, false, 0.0, 0.0, false, 1.0);  // no cursor: the file's start
    CHECK(Near(t.from, 0.0) && Near(t.cursor, 0.0));
    // A zone's play (to its chain end, not the file's end) does not loop without a time selection.
    t = PlanEditPlay(0.1, 0.4, false, 0.0, 0.0, true, 1.0);
    CHECK(!t.loops && Near(t.to, 0.4));
    // The cursor clamped into the file.
    t = PlanEditPlay(1.5, 2.0, false, 0.0, 0.0, false, 1.0);
    CHECK(Near(t.cursor, 1.0) && Near(t.to, 1.0));
    // With the zones: the time selection 0.2-0.5 over Heel [0.10, 0.30] · Toe [0.40, 0.60] plays 0.30 s, with
    // 0.10 s of silence.
    const SoundData d = Zoned({ZPart("heel", 0.10, 0.30), ZPart("toe", 0.40, 0.60)});
    t = PlanEditTransport(0.8, true, 0.2, 0.5, true, 1.0);
    const EditPlay p = EditPlayFrom(d, t.from, t.to);
    CHECK(p.voices.size() == 2 && Near(p.map.total, 0.30) && Near(p.voices[1].delay_ms, 200.0));
    CHECK(Near(EditPlayFrom(d, t.loop_a, t.loop_b).map.total, 0.30));
}

static void TestZonePlayRanges()
{
    // Heel [0.35, 0.40] · Gap [0.40, 0.45] · Heel [0.45, 0.70] · Toe [0.80, 0.90] · Gap [0.90, 0.95] · Toe [0.95, 1.00]
    const SoundData d = Zoned({ZPart("heel", 0.35, 0.40), ZGap(0.40, 0.45, 0.005, 0.005), ZPart("heel", 0.45, 0.70),
                               ZPart("toe", 0.80, 0.90), ZGap(0.90, 0.95, 0.005, 0.005), ZPart("toe", 0.95, 1.00)});
    const ZoneRef h1{0, 0}, g1{0, 1}, h2{0, 2}, t1{0, 3}, g2{0, 4}, t2{0, 5};
    CHECK(Near(ChainEnd(d, h1), 0.70) && Near(ChainEnd(d, h2), 0.70) && Near(ChainEnd(d, g1), 0.70));
    CHECK(Near(ChainEnd(d, t1), 1.00));
    // A zone's Play: its start to its chain's end (Gaps after it glued).
    EditRange r = ZonePlayRange(d, h1);
    CHECK(r.ok && Near(r.from, 0.35) && Near(r.to, 0.70));
    r = ZonePlayRange(d, h2);
    CHECK(r.ok && Near(r.from, 0.45) && Near(r.to, 0.70));
    // Hear the jump: max(left zone start, gap.a - 0.12) .. min(right zone end, gap.b + 0.12).
    r = ZonePlayRange(d, g1);
    CHECK(r.ok && Near(r.from, 0.35) && Near(r.to, 0.57));
    r = ZonePlayRange(d, g2);
    CHECK(r.ok && Near(r.from, 0.80) && Near(r.to, 1.00));
    // The selection's Play: from the first selected zone's start to the furthest chain end.
    r = SelectionPlayRange(d, {t1, h2});
    CHECK(r.ok && Near(r.from, 0.45) && Near(r.to, 1.00));
    r = SelectionPlayRange(d, {h1});
    CHECK(r.ok && Near(r.from, 0.35) && Near(r.to, 0.70));
    CHECK(!SelectionPlayRange(d, {}).ok && !ZonePlayRange(d, ZoneRef{0, 9}).ok);
    // Click plays: from the click to the chain's end; on a Gap the +-0.12 s window (inside the file).
    r = ClickPlayRange(d, h1, 0.37, 1.0);
    CHECK(r.ok && Near(r.from, 0.37) && Near(r.to, 0.70));
    r = ClickPlayRange(d, g1, 0.42, 1.0);
    CHECK(r.ok && Near(r.from, 0.28) && Near(r.to, 0.57));
    r = ClickPlayRange(d, g2, 0.92, 1.0);
    CHECK(r.ok && Near(r.from, 0.78) && Near(r.to, 1.00));
    // The zone play through the transport (no time selection, Loop on): it plays its chain, no loop.
    const EditTransport t = PlanEditPlay(ZonePlayRange(d, h1).from, ZonePlayRange(d, h1).to, false, 0, 0, true, 1.0);
    const EditPlay      p = EditPlayFrom(d, t.from, t.to);
    CHECK(!t.loops && p.voices.size() == 2 && Near(p.map.total, 0.055 + 0.255 - 0.010));
}

// ---- 13-62h review of Lots B / C ---------------------------------------------------------------------------

static bool NearMs(double a, double b) { return std::fabs(a - b) < 1e-3; }

// B1: a play started inside a Gap's crossfade (or its left part): the incoming voice never starts before the play,
// the two voices stay aligned (heard time), the heads and the engine's gains agree; a loop restart the same.
static void TestReviewPlayInCrossfade()
{
    const SoundData d = Zoned({ZPart("heel", 0.10, 0.40), ZGap(0.40, 0.45, 0.005, 0.005), ZPart("heel", 0.45, 0.70)});
    for (double A : {0.398, 0.402}) {
        const EditPlay p = EditPlayFrom(d, A, 0.70);
        CHECK(p.voices.size() == 2);
        if (p.voices.size() != 2) continue;
        const AuditionVoice &L = p.voices[0], &R = p.voices[1];
        CHECK(Near(L.delay_ms, 0.0) && Near(R.delay_ms, 0.0));  // never negative
        CHECK(Near(L.start_s, A) && Near(R.start_s, A + 0.05));
        CHECK(Near(R.fade_from_s, (A + 0.05) - 0.445) && Near(R.fade_in_s, 0.010));
        CHECK(Near(p.map.total, 0.70 - (A + 0.05)));
        // heard 1 ms in: two heads, 0.05 s apart in the file, their gains summing to 1, as the engine's
        const double t = 0.001;
        const std::vector<HeardHead> h = HeardHeads(p.map, t);
        CHECK(h.size() == 2);
        if (h.size() == 2) {
            CHECK(Near(h[0].file_t, A + t) && Near(h[1].file_t, A + t + 0.05) && Near(h[0].gain + h[1].gain, 1.0));
            const double gl = FadeGainAt(t - L.delay_ms / 1000.0 + L.fade_from_s, L.fade_span_s, L.fade_in_s, L.fade_out_s);
            const double gr = FadeGainAt(t - R.delay_ms / 1000.0 + R.fade_from_s, R.fade_span_s, R.fade_in_s, R.fade_out_s);
            CHECK(Near(gl, h[0].gain) && Near(gr, h[1].gain));
        }
    }
    // A loop over [0.402, 0.70] restarts with EditPlayFrom(loop_a, loop_b): the same voices each pass.
    const EditTransport tp = PlanEditPlay(0.402, 0.70, true, 0.402, 0.70, true, 1.0);
    CHECK(tp.loops && Near(EditPlayFrom(d, tp.loop_a, tp.loop_b).voices[1].delay_ms, 0.0));
}

// B2: the library tree's Play on a zoned Sound plays what Populate places: delays from from_anchor (heard time),
// each piece at its zone's gain on the root's volumes, the old frame / part gains not added.
static void TestReviewTreePlayZoned()
{
    SoundLibrary lib;
    SoundNode    n = Node("s", "", SoundNodeType::Sound, "S");
    n.volume_db = -3.0;
    n.sound = Zoned({ZPart("heel", 0.10, 0.40), ZGap(0.40, 0.45, 0.005, 0.005), ZPart("heel", 0.45, 0.70), ZPart("toe", 0.80, 0.90),
                     ZPart("heel", 1.00, 1.20)});
    n.sound.frames[0].zones[0].db = -2.0;
    n.sound.frames[0].zones[2].db = 1.5;
    PartSettings ps;
    ps.gain_db = -6.0;  // an old part gain: not added for zones
    SetPartSetting(n.sound, "heel", ps);
    lib.nodes.push_back(n);
    lib.root = "s";
    for (const std::string part : {"heel", "", "toe"}) {
        const std::vector<PieceApply>    pa = PieceApplyFor(lib.nodes[0].sound, 0, part);
        const std::vector<AuditionVoice> v = PartAuditionVoices(lib, lib.nodes[0], 0, part, 10.0);
        CHECK(!pa.empty() && v.size() == pa.size());
        for (size_t k = 0; k < v.size() && k < pa.size(); ++k) {
            CHECK(NearMs(v[k].delay_ms, 10.0 + pa[k].from_anchor * 1000.0));
            CHECK(Near(v[k].gain_db, -3.0 + pa[k].db));
            CHECK(Near(v[k].start_s, pa[k].slice.start_offs) && Near(v[k].length_s, pa[k].slice.length));
            CHECK(Near(v[k].fade_in_s, pa[k].fades.in) && Near(v[k].fade_out_s, pa[k].fades.out));
        }
    }
    const std::vector<AuditionVoice> h = PartAuditionVoices(lib, lib.nodes[0], 0, "heel");
    CHECK(h.size() == 3 && Near(h[1].delay_ms, 295.0) && Near(h[0].gain_db, -5.0) && Near(h[1].gain_db, -1.5));
    // the Sound's Play from the tree (PlanAudition) is built on them
    const AuditionPlan plan = PlanAudition(SoundStore{}, lib, "s", First());
    CHECK(!plan.voices.empty());
    for (const AuditionVoice& v : plan.voices) CHECK(v.gain_db < -0.9);  // never the old -6 dB part gain on top
}

// R3: a heard zone whose Gap leads to a muted / not soloed one starts (ends) at the Gap's edge with the At cuts fade.
static void TestReviewListenAcrossGap()
{
    SoundData d = Zoned({ZPart("heel", 0.10, 0.30), ZGap(0.30, 0.35, 0.004, 0.004), ZPart("toe", 0.35, 0.60)});
    d.cut_fade = 0.003;
    PartListen mute;
    mute.mute.insert("heel");
    EditPlay p = EditPlayFrom(d, 0.10, 0.60, &mute);
    CHECK(p.voices.size() == 1);
    if (p.voices.size() == 1) {
        CHECK(Near(p.voices[0].start_s, 0.35) && Near(p.voices[0].fade_in_s, 0.003) && Near(p.voices[0].fade_from_s, 0.0));
        CHECK(Near(p.voices[0].delay_ms, 200.0));  // the heard time kept: (0.304 - 0.10) - 0.008, then 0.004 later
    }
    CHECK(Near(p.map.total, 0.45));
    std::vector<HeardHead> h = HeardHeads(p.map, 0.198);  // the muted Heel's head alone in what was the crossfade
    CHECK(h.size() == 1 && !h[0].heard);
    PartListen solo;
    solo.solo.insert("heel");
    p = EditPlayFrom(d, 0.10, 0.60, &solo);
    CHECK(p.voices.size() == 1 && Near(p.voices[0].start_s + p.voices[0].length_s, 0.30) && Near(p.voices[0].fade_out_s, 0.003));
    // and Populate the same: a Toe event's item at the Gap's end, the At cuts fade in
    const std::vector<PieceApply> toe = PieceApplyFor(d, 0, "toe");
    CHECK(toe.size() == 1 && Near(toe[0].slice.start_offs, 0.35) && Near(toe[0].fades.in, 0.003));
}

// B3 / parity: a seeded mini-fuzz of zone layouts made with the Lot A gestures; for each variation, what Populate
// places (PieceApplyFor, a whole-variation and each part event) is what Edit plays (EditPlayFrom over that variation,
// the event's part soloed): the same file stretches, heard-time positions, fades and gains; no fade rescaled.
static void TestReviewPlaceEditParity()
{
    struct Rng {
        unsigned long long s;
        unsigned long long Next()
        {
            s ^= s << 13;
            s ^= s >> 7;
            s ^= s << 17;
            return s;
        }
        double U(double lo, double hi) { return lo + (hi - lo) * static_cast<double>(Next() % 1000000) / 1000000.0; }
        int    I(int n) { return n <= 0 ? 0 : static_cast<int>(Next() % static_cast<unsigned long long>(n)); }
    };
    Rng       r{0x62B0C0FFEEULL};
    int       bad = 0, compared = 0, gaps = 0;
    const std::vector<std::string> parts = {"heel", "toe"};
    for (int layout = 0; layout < 3000 && bad < 10; ++layout) {
        SoundData d = Zoned({ZPart("heel", 0.10, 0.90)});
        SoundFrame f2;
        f2.zones = {ZPart(r.I(2) ? "toe" : "heel", 1.10, 1.90)};
        f2.start = 1.10;
        f2.length = 0.80;
        d.frames.push_back(f2);
        d.cut_fade = r.U(0.0, 0.02);
        d.fade_in = r.U(0.0, 0.05);
        d.fade_out = r.U(0.0, 0.05);
        const ZoneContext ctx{nullptr, 3.0};
        const int         steps = 2 + r.I(10);
        for (int k = 0; k < steps; ++k) {
            const double t0 = r.U(0.05, 1.95), t1 = t0 + r.U(0.004, 0.15);
            const std::string part = parts[static_cast<size_t>(r.I(2))];
            const std::vector<ZoneRef> all = AllZones(d);
            const ZoneRef z = all.empty() ? ZoneRef{} : all[static_cast<size_t>(r.I(static_cast<int>(all.size())))];
            switch (r.I(9)) {
            case 0:
            case 1: d.gap_xfade = r.U(0.0, 0.12); MakeGap(d, t0, t1, ctx); break;
            case 2: SplitZoneAt(d, t0, ctx); break;
            case 3: NewZone(d, part, t0, t1, ctx); break;
            case 4: CutOut(d, t0, t0 + r.U(0.001, 0.05), ctx); break;
            case 5: GiveZonesPart(d, {z}, part, ctx); break;
            case 6: {
                const EditZone* q = ZoneOf(d, z);
                if (q && q->IsGap()) DragGapXfade(d, z, r.I(2) ? 1 : -1, r.U(q->a, q->b), r.I(2) == 0, ctx);
                break;
            }
            case 7: SetZoneFade(d, z, r.I(2) ? 1 : -1, r.U(0.0, 0.08), ctx); break;
            case 8: SetZoneDb(d, z, r.U(-12.0, 6.0), ctx); break;
            }
        }
        if (!ZonesProblem(d, 3.0).empty()) {
            if (bad < 5) std::printf("parity layout %d: %s\n", layout, ZonesProblem(d, 3.0).c_str());
            ++bad;
            continue;
        }
        gaps += GapCount(d) > 0 ? 1 : 0;
        for (size_t v = 0; v < d.frames.size(); ++v) {
            double a = 0.0, b = 0.0;
            if (!d.frames[v].on || !VariationEdges(d, static_cast<int>(v), &a, &b)) continue;
            bool foreign_gap = false;  // another variation's Gap inside this one: Edit jumps it, this placement does not
            for (size_t w = 0; w < d.frames.size(); ++w)
                for (const EditZone& q : d.frames[w].zones)
                    foreign_gap = foreign_gap || (w != v && d.frames[w].on && q.IsGap() && q.b > a && q.a < b);
            if (foreign_gap) continue;
            for (const std::string ev : {"", "heel", "toe"}) {
                const std::vector<PieceApply> pa = PieceApplyFor(d, static_cast<int>(v), ev);
                PartListen solo;
                if (!ev.empty()) solo.solo.insert(ev);
                EditPlay ep = EditPlayFrom(d, a, b, ev.empty() ? nullptr : &solo);
                ep.voices.erase(std::remove_if(ep.voices.begin(), ep.voices.end(),
                                               [&](const AuditionVoice& x) { return x.variation != static_cast<int>(v); }),
                                ep.voices.end());
                bool ok = pa.size() == ep.voices.size();
                for (size_t k = 0; ok && k < pa.size(); ++k) {
                    const AuditionVoice& vo = ep.voices[k];
                    ok = Near(vo.start_s, pa[k].slice.start_offs) && Near(vo.length_s, pa[k].slice.length) &&
                         NearMs(vo.delay_ms - ep.voices[0].delay_ms, pa[k].from_anchor * 1000.0) &&
                         Near(vo.fade_in_s, pa[k].fades.in) && Near(vo.fade_out_s, pa[k].fades.out) &&
                         Near(vo.gain_db, pa[k].db) && pa[k].fades.in + pa[k].fades.out <= pa[k].slice.length + 1e-9;
                }
                for (size_t k = 0; ok && k + 2 < ep.voices.size(); ++k)  // never three voices at once
                    ok = ep.voices[k + 2].delay_ms >= ep.voices[k].delay_ms + ep.voices[k].length_s * 1000.0 - 1e-6;
                ++compared;
                if (!ok) {
                    if (bad < 5) {
                        std::printf("parity layout %d variation %zu event '%s': placed != heard\n", layout, v, ev.c_str());
                        for (const EditZone& q : d.frames[v].zones)
                            std::printf("   zone %s [%.6f %.6f] xl %.6f xr %.6f fi %.4f fo %.4f\n", q.part.c_str(), q.a, q.b, q.xl, q.xr,
                                        q.fade_in ? *q.fade_in : -1.0, q.fade_out ? *q.fade_out : -1.0);
                        for (const PieceApply& x : pa)
                            std::printf("   piece [%.6f +%.6f] at %.6f in %.6f out %.6f db %.2f\n", x.slice.start_offs, x.slice.length,
                                        x.from_anchor, x.fades.in, x.fades.out, x.db);
                        for (const AuditionVoice& x : ep.voices)
                            std::printf("   voice [%.6f +%.6f] at %.6f in %.6f out %.6f db %.2f\n", x.start_s, x.length_s,
                                        x.delay_ms / 1000.0, x.fade_in_s, x.fade_out_s, x.gain_db);
                    }
                    ++bad;
                }
            }
        }
    }
    CHECK(bad == 0 && compared > 6000 && gaps > 1000);
    std::printf("place / edit parity: %d comparisons, %d layouts with Gaps\n", compared, gaps);
}

int main()
{
    TestReviewPlayInCrossfade();
    TestReviewTreePlayZoned();
    TestReviewListenAcrossGap();
    TestReviewPlaceEditParity();
    TestZonedEmptySpace();
    TestZonedGapCrossfade();
    TestZonedListen();
    TestEditTransport();
    TestZonePlayRanges();
    TestEditNav();
    TestPieceTrim();
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
