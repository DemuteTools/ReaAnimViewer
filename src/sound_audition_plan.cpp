// SPDX-License-Identifier: MIT
//
// See sound_audition_plan.h.

#include "sound_audition_plan.h"

#include <algorithm>
#include <cmath>

#include "sound_draw.h"
#include "sound_entries.h"
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
            const int v = picker.pick_variation ? picker.pick_variation(n) : -1;
            if (v < 0 || static_cast<size_t>(v) >= n.sound.frames.size()) return;
            const SoundFrame& f = n.sound.frames[static_cast<size_t>(v)];
            AuditionVoice voice;
            voice.node = n.uid;
            voice.variation = v;
            voice.start_s = f.start;
            voice.length_s = f.length;
            voice.delay_ms = delay_ms;
            voice.gain_db = GainFromRoot(lib, n) + f.db;
            AuditionFades(n.sound, v, &voice.fade_in_s, &voice.fade_out_s, &voice.fade_in_shape,
                          &voice.fade_out_shape);
            voice.path = std::move(path);
            plan.voices.push_back(std::move(voice));
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

std::vector<int> NormalizeSelection(const std::vector<SoundFrame>& frames, const std::vector<int>& selected)
{
    if (selected.empty()) return {};
    for (int i : selected)
        if (i >= 0 && static_cast<size_t>(i) < frames.size() && frames[static_cast<size_t>(i)].on) return selected;
    return {-1};  // only off frames selected: nothing
}

bool NormalizeTakeFrames(std::vector<SoundFrame>& frames, const std::vector<double>& peak_db, double target_db,
                         bool whole, const std::vector<int>& selection)
{
    auto ok = [&](int i) {
        return i >= 0 && static_cast<size_t>(i) < frames.size() && static_cast<size_t>(i) < peak_db.size() &&
               frames[static_cast<size_t>(i)].on && std::isfinite(peak_db[static_cast<size_t>(i)]);
    };
    std::vector<int> set;
    if (selection.empty()) {
        for (size_t i = 0; i < frames.size(); ++i)
            if (ok(static_cast<int>(i))) set.push_back(static_cast<int>(i));
    } else {
        for (int i : selection)
            if (ok(i)) set.push_back(i);
    }
    if (set.empty()) return false;
    double top = peak_db[static_cast<size_t>(set.front())];
    for (int i : set) top = std::max(top, peak_db[static_cast<size_t>(i)]);
    bool changed = false;
    for (int i : set) {
        SoundFrame&  f = frames[static_cast<size_t>(i)];
        const double db = target_db - (whole ? top : peak_db[static_cast<size_t>(i)]);
        if (std::fabs(f.db - db) > 1e-9 || !f.hand) changed = true;
        f.db = db;
        f.hand = true;
    }
    return changed;
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

}  // namespace rav
