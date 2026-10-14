// SPDX-License-Identifier: MIT
//
// See sound_audition_plan.h.

#include "sound_audition_plan.h"

#include <algorithm>
#include <cmath>

#include "sound_draw.h"
#include "sound_entries.h"
#include "sound_place_plan.h"  // 13-20 / 13-23: PieceApplyFor
#include "sound_record.h"
#include "sound_switch.h"

namespace rav {
namespace {

constexpr int kMaxDepth = 64;  // a cycle in a damaged library stops here
constexpr double kMinFrame = 0.001;

void Remember(std::vector<int>& h, int pick)
{
    if (pick < 0) return;
    h.push_back(pick);
    if (h.size() > 64) h.erase(h.begin());
}

LengthRange Range(const SoundStore& store, const SoundLibrary& lib, const SoundNode& n, int depth);

// A child a Random can draw: it has something to play (an empty one is never picked).
std::vector<const SoundNode*> PlayableChildren(const SoundStore& store, const SoundLibrary& lib, const SoundNode& n,
                                               int depth)
{
    std::vector<const SoundNode*> out;
    for (const SoundNode* k : ChildNodes(lib, n))
        if (Range(store, lib, *k, depth + 1).max_s > 0.0) out.push_back(k);
    return out;
}

struct Walker {
    const SoundStore&     store;
    const SoundLibrary&   lib;
    const AuditionPicker& picker;
    AuditionPlan&         plan;

    void Walk(const SoundNode& n, double delay_ms, std::vector<std::string> path, int depth)
    {
        if (depth > kMaxDepth) return;
        switch (n.type) {
        case SoundNodeType::Sound: {
            // 13-31: an audition is a combined event (no role): the whole variation, every Blend layer.
            const std::string part;
            const int v = picker.pick_part_variation ? picker.pick_part_variation(n, part)
                          : picker.pick_variation     ? picker.pick_variation(n)
                                                      : -1;
            if (v < 0 || static_cast<size_t>(v) >= n.sound.frames.size()) return;
            for (AuditionVoice& voice : PartAuditionVoices(lib, n, v, part, delay_ms)) {
                voice.path = path;
                plan.voices.push_back(std::move(voice));
            }
            return;
        }
        case SoundNodeType::Random: {
            const std::vector<const SoundNode*> kids = PlayableChildren(store, lib, n, depth);
            if (kids.empty()) return;
            const int i = picker.pick_child ? picker.pick_child(n, static_cast<int>(kids.size())) : 0;
            if (i < 0 || static_cast<size_t>(i) >= kids.size()) return;
            const SoundNode& k = *kids[static_cast<size_t>(i)];
            std::vector<std::string> p = path;
            p.push_back(k.uid);
            Walk(k, delay_ms, std::move(p), depth + 1);
            return;
        }
        case SoundNodeType::Blend:
            for (const SoundNode* k : BlendParts(lib, n)) {
                std::vector<std::string> p = path;
                p.push_back(k->uid);
                Walk(*k, delay_ms + std::max(0.0, k->offset_ms), std::move(p), depth + 1);
            }
            return;
        case SoundNodeType::Switch: {
            const std::string child = SwitchPreviewChild(store, lib, n);
            const SoundNode*  k = child.empty() ? nullptr : FindNode(lib, child);
            if (!k) return;
            std::vector<std::string> p = path;
            p.push_back(k->uid);
            Walk(*k, delay_ms, std::move(p), depth + 1);
            return;
        }
        }
    }
};

LengthRange Range(const SoundStore& store, const SoundLibrary& lib, const SoundNode& n, int depth)
{
    LengthRange r;
    if (depth > kMaxDepth) return r;
    switch (n.type) {
    case SoundNodeType::Sound: {
        bool first = true;
        for (const SoundFrame& f : n.sound.frames) {
            if (!f.on) continue;
            r.min_s = first ? f.length : std::min(r.min_s, f.length);
            r.max_s = first ? f.length : std::max(r.max_s, f.length);
            first = false;
        }
        return r;
    }
    case SoundNodeType::Random: {
        bool first = true;
        for (const SoundNode* k : ChildNodes(lib, n)) {
            const LengthRange c = Range(store, lib, *k, depth + 1);
            if (!(c.max_s > 0.0)) continue;  // never drawn (PlayableChildren)
            r.min_s = first ? c.min_s : std::min(r.min_s, c.min_s);
            r.max_s = first ? c.max_s : std::max(r.max_s, c.max_s);
            first = false;
        }
        return r;
    }
    case SoundNodeType::Blend:
        for (const SoundNode* k : BlendParts(lib, n)) {
            const LengthRange c = Range(store, lib, *k, depth + 1);
            const double      off = std::max(0.0, k->offset_ms) / 1000.0;
            r.min_s = std::max(r.min_s, off + c.min_s);
            r.max_s = std::max(r.max_s, off + c.max_s);
        }
        return r;
    case SoundNodeType::Switch: {
        const std::string child = SwitchPreviewChild(store, lib, n);
        const SoundNode*  k = child.empty() ? nullptr : FindNode(lib, child);
        return k ? Range(store, lib, *k, depth + 1) : r;
    }
    }
    return r;
}

bool Overlaps(const SoundFrame& a, const SoundFrame& b)
{
    return a.start < b.start + b.length && b.start < a.start + a.length;
}

}  // namespace

AuditionPicker AuditionHistory::Picker()
{
    AuditionPicker p;
    p.pick_child = [this](const SoundNode& n, int count) {
        std::vector<int> all;
        for (int i = 0; i < count; ++i) all.push_back(i);
        std::vector<int>& h = hist_[n.uid];
        const int pick = DrawPick(all, n.avoid_repeat, h, seed_, ++press_);
        Remember(h, pick);
        return pick;
    };
    p.pick_variation = [this](const SoundNode& n) {
        std::vector<int>& h = hist_[n.uid];
        const int pick = DrawPick(ActiveVariations(n.sound), n.avoid_repeat, h, seed_, ++press_);
        Remember(h, pick);
        return pick;
    };
    p.pick_part_variation = [this](const SoundNode& n, const std::string& part) {
        std::vector<int>& h = hist_[part.empty() ? n.uid : n.uid + "|" + part];
        const std::vector<int> active =
            n.sound.mode == DetectMode::Takes ? ActiveVariations(n.sound) : ActiveVariationsFor(n.sound, part);
        const int pick = DrawPick(active, n.avoid_repeat, h, seed_, ++press_);
        Remember(h, pick);
        return pick;
    };
    return p;
}

std::string SwitchPreviewChild(const SoundStore& store, const SoundLibrary& lib, const SoundNode& sw)
{
    (void)lib;
    bool               known = false;  // a deleted value, or one of another group: the default
    if (const SwitchGroup* g = sw.preview_value.empty() ? nullptr : FindGroup(store, sw.group_uid))
        for (const SwitchValue& v : g->values)
            if (v.uid == sw.preview_value) known = true;
    const std::string value = known ? sw.preview_value : DefaultValue(store, sw);
    if (value.empty()) return {};
    for (const SwitchCase& c : sw.cases)
        if (c.value_uid == value) return c.child_uid;
    return {};
}

AuditionPlan PlanAudition(const SoundStore& store, const SoundLibrary& lib, const std::string& node_uid,
                          const AuditionPicker& picker)
{
    AuditionPlan plan;
    plan.root = node_uid;
    if (const SoundNode* n = FindNode(lib, node_uid)) {
        Walker w{store, lib, picker, plan};
        w.Walk(*n, 0.0, {}, 0);
    }
    for (const AuditionVoice& v : plan.voices) plan.length_ms = std::max(plan.length_ms, v.delay_ms + v.length_s * 1000.0);
    plan.played = PlayedText(lib, plan.voices);
    return plan;
}

std::string PlayedText(const SoundLibrary& lib, const std::vector<AuditionVoice>& voices)
{
    std::string text;
    for (const AuditionVoice& v : voices) {
        if (v.piece > 0) continue;  // 13-20: the same variation's later piece
        std::string one;
        const std::vector<std::string> names = v.path.empty() ? std::vector<std::string>{v.node} : v.path;
        for (const std::string& uid : names) {
            const SoundNode* k = FindNode(lib, uid);
            if (!k) continue;
            if (!one.empty()) one += " \xE2\x80\xBA ";
            one += k->name;
        }
        if (!text.empty()) text += " + ";
        text += one;
    }
    return text.empty() ? std::string("nothing") : text;
}

std::vector<AuditionVoice> PartAuditionVoices(const SoundLibrary& lib, const SoundNode& n, int variation,
                                              const std::string& part_value, double delay_ms)
{
    std::vector<AuditionVoice> out;
    if (variation < 0 || static_cast<size_t>(variation) >= n.sound.frames.size()) return out;
    const SoundFrame& f = n.sound.frames[static_cast<size_t>(variation)];
    const double      gain = GainFromRoot(lib, n) + FrameGainDb(f, part_value);  // 13-28: the part's own gain
    if (n.sound.mode == DetectMode::Takes) {  // A8: the take whole
        AuditionVoice voice;
        voice.node = n.uid;
        voice.variation = variation;
        voice.start_s = f.start;
        voice.length_s = f.length;
        voice.delay_ms = delay_ms;
        voice.gain_db = gain;
        voice.mono = n.sound.mono;
        AuditionFades(n.sound, variation, &voice.fade_in_s, &voice.fade_out_s, &voice.fade_in_shape,
                      &voice.fade_out_shape);
        out.push_back(voice);
        return out;
    }
    const std::vector<PieceApply> pieces = PieceApplyFor(n.sound, variation, part_value, false);
    for (size_t k = 0; k < pieces.size(); ++k) {
        const PieceApply& pa = pieces[k];
        AuditionVoice     voice;
        voice.node = n.uid;
        voice.variation = variation;
        voice.start_s = pa.slice.start_offs;
        voice.length_s = pa.slice.length;
        voice.delay_ms = delay_ms + (pa.slice.start_offs - pieces.front().slice.start_offs) * 1000.0;
        voice.gain_db = gain;
        voice.fade_in_s = pa.fades.set_in ? pa.fades.in : 0.0;
        voice.fade_out_s = pa.fades.set_out ? pa.fades.out : 0.0;
        voice.fade_in_shape = pa.fades.set_in ? pa.fades.in_shape : 0;
        voice.fade_out_shape = pa.fades.set_out ? pa.fades.out_shape : 0;
        voice.piece = static_cast<int>(k);
        voice.mono = n.sound.mono;
        out.push_back(voice);
    }
    return out;
}

AuditionVoice ZoneAuditionVoice(const SoundLibrary& lib, const SoundNode& n, double start, double length)
{
    AuditionVoice voice;
    voice.node = n.uid;
    voice.mono = n.sound.mono;
    if (!std::isfinite(start) || !std::isfinite(length) || !(length > 0.0)) return voice;
    voice.start_s = std::max(0.0, start);
    voice.length_s = start + length - voice.start_s;
    if (!(voice.length_s > 0.0)) {
        voice.length_s = 0.0;
        return voice;
    }
    double db = 0.0;
    for (size_t i = 0; i < n.sound.frames.size(); ++i) {
        const SoundFrame& f = n.sound.frames[i];
        if (voice.start_s >= f.start && voice.start_s < f.start + f.length) {
            std::string own;  // 13-28: the part there plays at its own gain
            db = PartAt(n.sound, static_cast<int>(i), voice.start_s, &own) ? FrameGainDb(f, own) : f.db;
            voice.variation = static_cast<int>(i);
            break;
        }
    }
    voice.gain_db = GainFromRoot(lib, n) + db;
    const double cut = std::isfinite(n.sound.cut_fade) && n.sound.cut_fade > 0.0 ? n.sound.cut_fade : 0.0;
    voice.fade_in_s = voice.fade_out_s = std::min(cut, voice.length_s / 2.0);
    return voice;
}

double GainFromRoot(const SoundLibrary& lib, const SoundNode& n)
{
    double g = 0.0;
    const SoundNode* p = &n;
    for (int guard = 0; p && guard < kMaxDepth; ++guard) {
        g += p->volume_db;
        if (p->parent.empty()) break;
        p = FindNode(lib, p->parent);
    }
    return g;
}

std::set<std::string> LitAt(const AuditionPlan& plan, double t_ms)
{
    std::set<std::string> lit;
    for (const AuditionVoice& v : plan.voices) {
        if (t_ms < v.delay_ms || t_ms >= v.delay_ms + v.length_s * 1000.0) continue;
        lit.insert(plan.root);
        lit.insert(v.path.begin(), v.path.end());
    }
    return lit;
}

LengthRange NodeLengthRange(const SoundStore& store, const SoundLibrary& lib, const std::string& uid)
{
    const SoundNode* n = FindNode(lib, uid);
    return n ? Range(store, lib, *n, 0) : LengthRange{};
}

std::vector<SoundFrame> MergeHandFrames(const std::vector<SoundFrame>& detected,
                                        const std::vector<SoundFrame>& previous, bool by_index)
{
    if (by_index) {
        std::vector<SoundFrame> out = detected;
        for (size_t i = 0; i < previous.size() && i < out.size(); ++i)
            if (previous[i].hand) out[i] = previous[i];
        return out;
    }
    std::vector<SoundFrame> out;
    for (const SoundFrame& d : detected) {
        bool hit = false;
        for (const SoundFrame& p : previous)
            if (p.hand && Overlaps(d, p)) hit = true;
        if (!hit) out.push_back(d);
    }
    for (const SoundFrame& p : previous)
        if (p.hand) out.push_back(p);
    std::stable_sort(out.begin(), out.end(), [](const SoundFrame& a, const SoundFrame& b) { return a.start < b.start; });
    return out;
}

bool TrimTakeFrame(SoundData& data, int i, double start, double length, double take_source_len)
{
    if (i < 0 || static_cast<size_t>(i) >= data.frames.size() || !(take_source_len > 0.0)) return false;
    double s = std::max(0.0, start);
    double e = std::min(take_source_len, start + length);
    if (e - s <= kMinFrame) return false;
    SoundFrame& f = data.frames[static_cast<size_t>(i)];
    if (std::fabs(f.start - s) < 1e-9 && std::fabs(f.length - (e - s)) < 1e-9) return false;
    f.start = s;
    f.length = e - s;
    f.hand = true;
    data.edited = true;
    return true;
}

bool AuditionEnded(double pos, double length, double stalled_s)
{
    if (!(length > 0.0)) return true;
    if (pos >= length) return true;  // stopped exactly at its end; short of it, the stall rule ends it
    return pos > 0.0 && stalled_s >= 0.5;
}

bool FrameEditAllowed(DetectMode mode, FrameEdit edit)
{
    if (mode == DetectMode::Takes) return edit != FrameEdit::Add && edit != FrameEdit::Split;
    return true;
}

EditTrimPlan PlanEditTrim(const std::vector<SoundFrame>& frames, const std::vector<int>& selected, EditTrimKind kind,
                          double cursor, double zone_start, double zone_len)
{
    EditTrimPlan plan;
    const bool   zone = kind == EditTrimKind::ToZone;
    if (zone && !(zone_len > 0.0)) {
        plan.why = "Select a zone first: drag across the waveform";
        return plan;
    }
    if (!zone && !(cursor >= 0.0)) {
        plan.why = "Click the waveform first: the edit cursor sets where to trim";
        return plan;
    }
    const double ze = zone_start + zone_len;
    auto valid = [&](int i) { return i >= 0 && static_cast<size_t>(i) < frames.size(); };
    // The variations it may touch: the selected ones, else every one (the cursor / zone picks among them).
    std::vector<int> pool;
    for (int i : selected)
        if (valid(i) && std::find(pool.begin(), pool.end(), i) == pool.end()) pool.push_back(i);
    const bool any_selected = !pool.empty();
    if (!any_selected)
        for (size_t i = 0; i < frames.size(); ++i) pool.push_back(static_cast<int>(i));
    std::sort(pool.begin(), pool.end());
    bool touched = false;  // a variation the cursor / zone is on (even if nothing changes)
    for (int i : pool) {
        const SoundFrame& f = frames[static_cast<size_t>(i)];
        const double      fe = f.start + f.length;
        double            s = f.start, e = fe;
        if (zone) {
            if (!(ze > f.start && zone_start < fe)) continue;  // the zone does not cross it
            s = std::max(f.start, zone_start);
            e = std::min(fe, ze);
        } else {
            if (!(cursor > f.start && cursor < fe)) continue;  // the cursor is not inside it
            if (kind == EditTrimKind::LeftOfCursor) s = cursor;
            else e = cursor;
        }
        touched = true;
        if (e - s <= kMinFrame) continue;  // nothing would be left: unchanged
        if (std::fabs(s - f.start) < 1e-9 && std::fabs(e - fe) < 1e-9) continue;  // already so
        plan.steps.push_back(EditTrimStep{i, s, e - s});
    }
    if (plan.steps.empty()) {
        if (touched) plan.why = zone ? "Already inside the zone" : "Nothing to trim there";
        else if (zone) plan.why = any_selected ? "The zone crosses no selected variation" : "The zone crosses no variation";
        else plan.why = any_selected ? "The edit cursor is not inside a selected variation" : "The edit cursor is not inside a variation";
    }
    return plan;
}

EditSplitPlan PlanEditSplit(const std::vector<SoundFrame>& frames, const std::vector<int>& selected, double cursor)
{
    EditSplitPlan plan;
    if (!(cursor >= 0.0)) {
        plan.why = "Click the waveform first: the edit cursor sets where to split";
        return plan;
    }
    auto valid = [&](int i) { return i >= 0 && static_cast<size_t>(i) < frames.size(); };
    std::vector<int> pool;  // the selected ones, else every one (the cursor picks among them)
    for (int i : selected)
        if (valid(i) && std::find(pool.begin(), pool.end(), i) == pool.end()) pool.push_back(i);
    const bool any_selected = !pool.empty();
    if (!any_selected)
        for (size_t i = 0; i < frames.size(); ++i) pool.push_back(static_cast<int>(i));
    std::sort(pool.begin(), pool.end());
    bool touched = false;
    for (int i : pool) {
        const SoundFrame& f = frames[static_cast<size_t>(i)];
        const double      fe = f.start + f.length;
        if (!(cursor > f.start && cursor < fe)) continue;  // the cursor is not inside it
        touched = true;
        if (cursor - f.start <= kMinFrame || fe - cursor <= kMinFrame) continue;  // a piece would be empty (SplitFrame)
        plan.frames.push_back(i);
    }
    if (plan.frames.empty())
        plan.why = touched ? "Too close to the variation's edge to split"
                           : any_selected ? "The edit cursor is not inside a selected variation"
                                          : "The edit cursor is not inside a variation";
    return plan;
}

std::vector<int> NormalizeSelection(const std::vector<SoundFrame>& frames, const std::vector<int>& selected)
{
    if (selected.empty()) return {};
    for (int i : selected)
        if (i >= 0 && static_cast<size_t>(i) < frames.size() && frames[static_cast<size_t>(i)].on) return selected;
    return {-1};  // only off frames selected: nothing
}

std::string VariationCountText(int detected, const std::vector<SoundFrame>& frames)
{
    const int n = static_cast<int>(frames.size());
    int       off = 0;
    for (const SoundFrame& f : frames)
        if (!f.on) ++off;
    std::string s;
    if (detected != n) s = std::to_string(detected) + " \xE2\x86\x92 ";
    s += std::to_string(n) + (n == 1 ? " variation" : " variations");
    if (off > 0) s += " \xC2\xB7 " + std::to_string(off) + " off";
    return s;
}

void AuditionFades(const SoundData& data, int i, double* in_s, double* out_s, int* in_shape, int* out_shape,
                   bool folder_source)
{
    double a = 0.0, b = 0.0;
    int    sa = 0, sb = 0;
    if (!folder_source && i >= 0 && i < static_cast<int>(data.frames.size())) {
        const FrameFades f = EffectiveFades(data, data.frames[static_cast<size_t>(i)]);
        const bool       whole = data.mode == DetectMode::Whole;
        if (!whole || f.own_in) {
            a = f.in;
            sa = f.in_shape;
        }
        if (!whole || f.own_out) {
            b = f.out;
            sb = f.out_shape;
        }
    }
    if (in_s) *in_s = a;
    if (out_s) *out_s = b;
    if (in_shape) *in_shape = sa;
    if (out_shape) *out_shape = sb;
}

namespace {
// A fade-in curve over x in [0, 1] (0 -> 0, 1 -> 1).
double FadeCurve(double x, int shape)
{
    x = std::min(std::max(x, 0.0), 1.0);
    switch (shape) {
    case 1: return 1.0 - (1.0 - x) * (1.0 - x);
    case 2: return x * x;
    case 3: { const double y = 1.0 - x; return 1.0 - y * y * y * y; }
    case 4: return x * x * x * x;
    case 5: return x * x * (3.0 - 2.0 * x);
    default: return x;
    }
}
}  // namespace

double FadeGainAt(double t, double length, double in, double out, int in_shape, int out_shape)
{
    if (!std::isfinite(t) || !std::isfinite(length) || length <= 0.0) return 1.0;
    double a = std::isfinite(in) ? std::max(in, 0.0) : 0.0;
    double b = std::isfinite(out) ? std::max(out, 0.0) : 0.0;
    if (a + b > length) {
        const double k = length / (a + b);
        a *= k;
        b *= k;
    }
    double g = 1.0;
    if (a > 0.0 && t < a) g *= FadeCurve(t / a, in_shape);
    if (b > 0.0 && t > length - b) g *= FadeCurve((length - t) / b, out_shape);
    return g;
}

namespace {

// 13-54: a variation's whole voices kept to what its parts play once their margins apply (`pieces`,
// PartPieces; touching pieces are one stretch): a voice's own fade stays on a side it keeps, a side the
// margins cut gets the Sound's cut fade (as Populate gives a part's cut side).
std::vector<AuditionVoice> MarginVoices(const std::vector<AuditionVoice>& voices, const std::vector<PartSpan>& pieces,
                                        const SoundData& d)
{
    std::vector<std::pair<double, double>> runs;
    for (const PartSpan& sp : pieces) {
        if (!runs.empty() && std::fabs(runs.back().second - sp.start) <= 1e-9)
            runs.back().second = sp.start + sp.length;
        else
            runs.push_back({sp.start, sp.start + sp.length});
    }
    const double               cut = std::isfinite(d.cut_fade) && d.cut_fade > 0.0 ? d.cut_fade : 0.0;
    std::vector<AuditionVoice> out;
    for (const AuditionVoice& v : voices) {
        const double s0 = v.start_s, s1 = v.start_s + v.length_s;
        for (const auto& r : runs) {
            const double a = std::max(s0, r.first), b = std::min(s1, r.second);
            if (b - a <= 1e-9) continue;
            AuditionVoice w = v;
            w.start_s = a;
            w.length_s = b - a;
            if (a > s0 + 1e-9) {
                w.fade_in_s = cut;
                w.fade_in_shape = 0;
            }
            if (b < s1 - 1e-9) {
                w.fade_out_s = cut;
                w.fade_out_shape = 0;
            }
            out.push_back(w);
        }
    }
    return out;
}

}  // namespace

std::vector<AuditionVoice> EditPlayVoices(const SoundLibrary& lib, const SoundNode& n, int take)
{
    std::vector<AuditionVoice> out;
    const SoundData&           d = n.sound;
    if (d.mode == DetectMode::Takes) {
        if (take < 0 || static_cast<size_t>(take) >= d.frames.size() || !d.frames[static_cast<size_t>(take)].on) return out;
        return PartAuditionVoices(lib, n, take, "");
    }
    std::vector<int> order;
    for (size_t i = 0; i < d.frames.size(); ++i)
        if (d.frames[i].on && d.frames[i].length > 0.0) order.push_back(static_cast<int>(i));
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return d.frames[static_cast<size_t>(a)].start < d.frames[static_cast<size_t>(b)].start;
    });
    const double root = GainFromRoot(lib, n);
    double       covered = -1e300;  // the file is heard up to here (the earlier variation wins)
    auto         push = [&](AuditionVoice v) {
        const double end = v.start_s + v.length_s;
        if (end <= covered + 1e-9) return;
        if (v.start_s < covered) {
            const double cut = covered - v.start_s;
            if (!(v.fade_span_s > 0.0)) v.fade_span_s = v.length_s;
            v.start_s += cut;
            v.length_s -= cut;
            v.fade_from_s += cut;
        }
        v.delay_ms = v.start_s * 1000.0;  // the file's clock
        out.push_back(std::move(v));
    };
    for (int i : order) {
        const SoundFrame& f = d.frames[static_cast<size_t>(i)];
        const bool        by_part = IsDivided(d) && !f.part_db.empty();
        // 13-54: with the parts' margins, only what the parts play (PartPieces), owned by the part playing it.
        const bool                  margins = PartMarginsApply(d);
        const std::vector<PartSpan> owners = margins ? PartPieces(d, i) : std::vector<PartSpan>();
        const std::vector<AuditionVoice> whole =
            margins ? MarginVoices(PartAuditionVoices(lib, n, i, ""), owners, d) : PartAuditionVoices(lib, n, i, "");
        auto owner_at = [&](double t, std::string* own) {
            if (!margins) return PartAt(d, i, t, own);
            for (const PartSpan& sp : owners)
                if (t >= sp.start && t < sp.start + sp.length) {
                    *own = sp.value_uid;
                    return true;
                }
            return false;
        };
        for (const AuditionVoice& piece : whole) {
            if (!by_part) {
                push(piece);
                continue;
            }
            // 13-28: the piece cut where its owner changes (a cut, a zone edge), each stretch at its part's gain.
            const double        s0 = piece.start_s, s1 = piece.start_s + piece.length_s;
            std::vector<double> b = {s0, s1};
            for (double c : f.cuts)
                if (c > s0 && c < s1) b.push_back(c);
            for (const SoundZone& z : d.zones)
                for (double t : {z.start, z.start + z.length})
                    if (t > s0 && t < s1) b.push_back(t);
            for (const PartSpan& sp : owners)  // 13-54: where the playing part changes
                for (double t : {sp.start, sp.start + sp.length})
                    if (t > s0 && t < s1) b.push_back(t);
            std::sort(b.begin(), b.end());
            AuditionVoice cur;
            bool          open = false;
            for (size_t k = 0; k + 1 < b.size(); ++k) {
                if (b[k + 1] - b[k] <= 1e-9) continue;
                std::string  own;
                const double db = root + (owner_at((b[k] + b[k + 1]) * 0.5, &own) ? FrameGainDb(f, own) : f.db);
                if (open && std::fabs(cur.gain_db - db) < 1e-12) {
                    cur.length_s = b[k + 1] - cur.start_s;
                    continue;
                }
                if (open) push(cur);
                cur = piece;
                cur.start_s = b[k];
                cur.length_s = b[k + 1] - b[k];
                cur.gain_db = db;
                cur.fade_from_s = b[k] - s0;
                cur.fade_span_s = piece.length_s;
                open = true;
            }
            if (open) {
                if (std::fabs(cur.start_s - s0) < 1e-12 && std::fabs(cur.length_s - piece.length_s) < 1e-12) {
                    cur.fade_from_s = 0.0;  // one part all along: the piece itself
                    cur.fade_span_s = 0.0;
                }
                push(cur);
            }
        }
        covered = std::max(covered, f.start + f.length);
    }
    return out;
}

std::vector<AuditionVoice> VoicesFrom(const std::vector<AuditionVoice>& voices, double from_s)
{
    if (!(from_s > 0.0) || !std::isfinite(from_s)) return voices;
    const double               from_ms = from_s * 1000.0;
    std::vector<AuditionVoice> out;
    for (AuditionVoice v : voices) {
        const double end_ms = v.delay_ms + v.length_s * 1000.0;
        if (end_ms <= from_ms + 1e-6) continue;
        if (v.delay_ms < from_ms) {
            const double cut = (from_ms - v.delay_ms) / 1000.0;
            if (!(v.fade_span_s > 0.0)) v.fade_span_s = v.length_s;
            v.start_s += cut;
            v.length_s -= cut;
            v.fade_from_s += cut;
            v.delay_ms = 0.0;
        } else {
            v.delay_ms -= from_ms;
        }
        out.push_back(std::move(v));
    }
    return out;
}

bool SettleEditPlay(EditPlaySettle& s, const std::vector<AuditionVoice>& now, const std::vector<AuditionVoice>& playing,
                    double t, double settle_s)
{
    if (SameVoices(now, playing)) {
        s.since = -1.0;
        s.seen.clear();
        return false;
    }
    if (s.since < 0.0 || !SameVoices(now, s.seen)) {
        s.seen = now;
        s.since = t;
        return false;
    }
    if (t - s.since < settle_s) return false;
    s.since = -1.0;
    s.seen.clear();
    return true;
}

bool SameVoices(const std::vector<AuditionVoice>& a, const std::vector<AuditionVoice>& b)
{
    if (a.size() != b.size()) return false;
    auto eq = [](double x, double y) { return std::fabs(x - y) <= 1e-9; };
    for (size_t i = 0; i < a.size(); ++i) {
        const AuditionVoice& x = a[i];
        const AuditionVoice& y = b[i];
        if (x.node != y.node || x.variation != y.variation || !eq(x.start_s, y.start_s) || !eq(x.length_s, y.length_s) ||
            !eq(x.delay_ms, y.delay_ms) || !eq(x.gain_db, y.gain_db) || !eq(x.fade_in_s, y.fade_in_s) ||
            !eq(x.fade_out_s, y.fade_out_s) || x.fade_in_shape != y.fade_in_shape || x.fade_out_shape != y.fade_out_shape ||
            !eq(x.fade_from_s, y.fade_from_s) || !eq(x.fade_span_s, y.fade_span_s) || x.mono != y.mono)
            return false;
    }
    return true;
}

double MonoOf(double l, double r, MonoMode mono)
{
    switch (mono) {
    case MonoMode::Right: return r;
    case MonoMode::Sum: return (l + r) * 0.5;
    case MonoMode::Left:
    case MonoMode::Off: break;
    }
    return l;
}

AuditionPlan PlanChildAudition(const SoundStore& store, const SoundLibrary& lib, const std::string& parent_uid,
                               const std::string& child_uid, const AuditionPicker& picker)
{
    const SoundNode* k = FindNode(lib, child_uid);
    if (!k || k->parent != parent_uid) {
        AuditionPlan none;
        none.root = parent_uid;
        none.played = "nothing";
        return none;
    }
    AuditionPlan plan = PlanAudition(store, lib, child_uid, picker);
    plan.root = parent_uid;
    for (AuditionVoice& v : plan.voices) v.path.insert(v.path.begin(), child_uid);
    plan.played = PlayedText(lib, plan.voices);
    return plan;
}

}  // namespace rav
