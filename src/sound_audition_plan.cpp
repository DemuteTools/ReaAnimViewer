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
        const std::vector<int> active = ActiveVariationsFor(n.sound, part);  // 13-62i: Takes mode too
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
    // 13-62h review (B2) / 13-62i: every Sound plays as Populate places it, from its zones (Takes mode too: its take's
    // zones): each piece at its zone's gain (PieceApply::db) on the root's volumes, spaced in heard time.
    const double      gain = GainFromRoot(lib, n);
    const MonoMode    mono = PartMono(n.sound, part_value);                        // 13-62: its own, else the Sound's
    const std::vector<PieceApply> pieces = PieceApplyFor(n.sound, variation, part_value, false);
    for (size_t k = 0; k < pieces.size(); ++k) {
        const PieceApply& pa = pieces[k];
        AuditionVoice     voice;
        voice.node = n.uid;
        voice.variation = variation;
        voice.start_s = pa.slice.start_offs;
        voice.length_s = pa.slice.length;
        voice.delay_ms = delay_ms + (pa.from_anchor - pieces.front().from_anchor) * 1000.0;
        voice.gain_db = gain + pa.db;
        voice.fade_in_s = pa.fades.set_in ? pa.fades.in : 0.0;
        voice.fade_out_s = pa.fades.set_out ? pa.fades.out : 0.0;
        voice.fade_in_shape = pa.fades.set_in ? pa.fades.in_shape : 0;
        voice.fade_out_shape = pa.fades.set_out ? pa.fades.out_shape : 0;
        voice.piece = static_cast<int>(k);
        voice.mono = mono;
        voice.part = part_value;
        out.push_back(voice);
    }
    return out;
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

bool AuditionEnded(double pos, double length, double stalled_s)
{
    if (!(length > 0.0)) return true;
    if (pos >= length) return true;  // stopped exactly at its end; short of it, the stall rule ends it
    return pos > 0.0 && stalled_s >= 0.5;
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


bool PartHeard(const PartListen& listen, const std::string& value)
{
    if (!listen.solo.empty()) return listen.solo.count(value) > 0;
    return listen.mute.count(value) == 0;
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

// ---- 13-62h (Lot C): a zoned Sound's Edit playback ---------------------------------------------------------

namespace {

constexpr double kJumpWindow = 0.12;  // "Hear the jump": this much of each side of a Gap (mockup playZone)

const SoundFrame* FrameOfZone(const SoundData& d, const ZoneRef& r)
{
    if (r.frame < 0 || static_cast<size_t>(r.frame) >= d.frames.size()) return nullptr;
    return &d.frames[static_cast<size_t>(r.frame)];
}

}  // namespace

EditPlay EditPlayFrom(const SoundData& d, double from, double to, const PartListen* listen, int tl)
{
    EditPlay out;
    if (!UsesZones(d) || !std::isfinite(from) || !std::isfinite(to) || !(to > from)) return out;
    const bool         listening = listen && listen->Active();
    double             at = 0.0;
    std::map<int, int> nth;  // voices so far per variation (PlayedText names a variation's first one)
    for (const HeardPiece& p : HeardPieces(d, from, to, tl)) {
        HeardSpan s;
        s.at = at;
        s.a = s.lo = p.a;
        s.b = s.hi = p.b;
        const EditZone* z = ZoneOf(d, p.z);
        if (z && !z->IsGap()) {
            s.z = p.z;
            // The envelope of the zone's whole heard stretch (ZoneEnv): a side a Gap holds ramps over its crossfade.
            const ZoneRef   lgr = GapBefore(d, p.z), rgr = GapAfter(d, p.z);
            const EditZone* lg = ZoneOf(d, lgr);
            const EditZone* rg = ZoneOf(d, rgr);
            const SoundFrame* f = FrameOfZone(d, p.z);
            auto heard_part = [&](const ZoneRef& q) {
                const EditZone* qz = ZoneOf(d, q);
                return qz && (!listening || PartHeard(*listen, qz->part));
            };
            s.heard = f && f->on && (!listening || PartHeard(*listen, z->part));
            // 13-62h review (R3): a heard zone whose Gap leads to a silent one (muted / not soloed) does not play into
            // the Gap: that side is cut at the Gap's edge with the At cuts fade (as Populate does when the zone
            // across is not placed); its heard time is kept.
            const bool cut_in = s.heard && lg && !heard_part(GapLeft(d, lgr));
            const bool cut_out = s.heard && rg && !heard_part(GapRight(d, rgr));
            s.lo = lg && !cut_in ? lg->b - lg->xr : z->a;
            s.hi = rg && !cut_out ? rg->a + rg->xl : z->b;
            s.fin = lg && !cut_in ? lg->xl + lg->xr : (cut_in ? ZoneCutFade(d, p.z, -1).s : ZoneFade(d, p.z, -1).s);
            s.fout = rg && !cut_out ? rg->xl + rg->xr : (cut_out ? ZoneCutFade(d, p.z, 1).s : ZoneFade(d, p.z, 1).s);
            if (cut_in && s.a < z->a) {
                s.at += z->a - s.a;
                s.a = z->a;
            }
            if (cut_out && s.b > z->b) s.b = z->b;
            if (s.heard && !(s.b > s.a)) s.heard = false;  // only its jumped stretch was in the range
            if (s.heard) {
                AuditionVoice v;
                v.variation = p.z.frame;
                v.start_s = s.a;
                v.length_s = s.b - s.a;
                v.delay_ms = s.at * 1000.0;
                v.gain_db = z->db;
                v.fade_in_s = s.fin;
                v.fade_out_s = s.fout;
                // 13-62i: a zone's own fade keeps its shape, as Populate places it (a Gap's crossfade, At cuts: linear).
                if (!lg && !cut_in && ZoneFade(d, p.z, -1).src == FadeSrc::Own) v.fade_in_shape = z->fade_in_shape;
                if (!rg && !cut_out && ZoneFade(d, p.z, 1).src == FadeSrc::Own) v.fade_out_shape = z->fade_out_shape;
                v.fade_from_s = s.a - s.lo;
                v.fade_span_s = s.hi - s.lo;
                v.piece = nth[p.z.frame]++;
                v.mono = PartMono(d, z->part);
                v.part = z->part;
                out.voices.push_back(std::move(v));
            }
        }
        out.map.spans.push_back(s);
        out.map.total = std::max(out.map.total, at + (p.b - p.a));
        at += (p.b - p.a) - p.xo;
    }
    return out;
}

EditPlay EditPlayFrom(const SoundLibrary& lib, const SoundNode& n, double from, double to, const PartListen* listen,
                      int tl)
{
    EditPlay     out = EditPlayFrom(n.sound, from, to, listen, tl);
    const double root = GainFromRoot(lib, n);
    for (AuditionVoice& v : out.voices) {
        v.node = n.uid;
        v.gain_db += root;
    }
    return out;
}

std::vector<HeardHead> HeardHeads(const HeardMap& map, double heard_t)
{
    std::vector<HeardHead> out;
    if (!std::isfinite(heard_t)) return out;
    for (const HeardSpan& s : map.spans) {
        if (heard_t < s.at || heard_t >= s.at + (s.b - s.a)) continue;
        HeardHead h;
        h.file_t = s.a + (heard_t - s.at);
        h.heard = s.heard;
        h.z = s.z;
        if (s.z.frame >= 0) {
            const double t = h.file_t;
            double       g = 1.0;
            if (s.fin > 0.0 && t < s.lo + s.fin) g *= (t - s.lo) / s.fin;
            if (s.fout > 0.0 && t > s.hi - s.fout) g *= (s.hi - t) / s.fout;
            h.gain = std::min(std::max(g, 0.0), 1.0);
        }
        h.incoming = !out.empty();  // the later span of a crossfade
        out.push_back(h);
    }
    return out;
}

double ChainEnd(const SoundData& d, const ZoneRef& z)
{
    const EditZone* q = ZoneOf(d, z);
    if (!q) return 0.0;
    const std::vector<ZoneRef> c = ChainOf(d, z);
    const EditZone*            last = c.empty() ? nullptr : ZoneOf(d, c.back());
    return last ? last->b : q->b;
}

EditRange ZonePlayRange(const SoundData& d, const ZoneRef& z)
{
    EditRange       r;
    const EditZone* q = ZoneOf(d, z);
    if (!q) return r;
    if (q->IsGap()) {
        const EditZone* l = ZoneOf(d, GapLeft(d, z));
        const EditZone* rt = ZoneOf(d, GapRight(d, z));
        r.from = std::max(l ? l->a : q->a, q->a - kJumpWindow);
        r.to = std::min(rt ? rt->b : q->b, q->b + kJumpWindow);
    } else {
        r.from = q->a;
        r.to = ChainEnd(d, z);
    }
    r.ok = r.to > r.from;
    return r;
}

EditRange SelectionPlayRange(const SoundData& d, const std::vector<ZoneRef>& sel)
{
    std::vector<ZoneRef> L;
    for (const ZoneRef& z : sel)
        if (ZoneOf(d, z)) L.push_back(z);
    if (L.empty()) return {};
    if (L.size() == 1) return ZonePlayRange(d, L.front());
    EditRange r;
    r.from = 1e300;
    r.to = -1e300;
    for (const ZoneRef& z : L) {
        r.from = std::min(r.from, ZoneOf(d, z)->a);
        r.to = std::max(r.to, ChainEnd(d, z));
    }
    r.ok = r.to > r.from;
    return r;
}

EditRange ClickPlayRange(const SoundData& d, const ZoneRef& z, double t, double file_len)
{
    EditRange       r;
    const EditZone* q = ZoneOf(d, z);
    if (!q || !std::isfinite(t)) return r;
    if (q->IsGap()) {
        r.from = std::max(0.0, q->a - kJumpWindow);
        r.to = q->b + kJumpWindow;
        if (file_len > 0.0) r.to = std::min(r.to, file_len);
    } else {
        r.from = std::min(std::max(t, q->a), q->b);
        r.to = ChainEnd(d, z);
    }
    r.ok = r.to > r.from;
    return r;
}

EditTransport PlanEditPlay(double from, double to, bool has_tsel, double tsel_a, double tsel_b, bool loop, double file_len)
{
    EditTransport tp;
    const double  f0 = std::isfinite(from) ? from : 0.0;
    const double  T = file_len > 0.0 ? file_len
                                     : std::max({std::isfinite(to) ? to : 0.0, f0, has_tsel && std::isfinite(tsel_b) ? tsel_b : 0.0});
    const double  cur = std::min(std::max(f0, 0.0), T);
    tp.cursor = tp.from = cur;
    tp.to = std::isfinite(to) ? std::min(to, T) : T;
    if (has_tsel && tsel_b > tsel_a) {
        const double a = std::max(tsel_a, 0.0), b = std::min(tsel_b, T);
        if (cur < a - kZoneTouch || cur >= b) tp.cursor = tp.from = a;
        tp.to = b;
        if (loop) {
            tp.loops = true;
            tp.loop_a = a;
            tp.loop_b = b;
        }
    } else if (loop && tp.to >= T - 1e-9) {
        tp.loops = true;
        tp.loop_a = 0.0;
        tp.loop_b = T;
    }
    return tp;
}

EditTransport PlanEditTransport(double cursor, bool has_tsel, double tsel_a, double tsel_b, bool loop, double file_len)
{
    return PlanEditPlay(cursor < 0.0 ? 0.0 : cursor, file_len, has_tsel, tsel_a, tsel_b, loop, file_len);
}

bool EditLoopDue(const EditTransport& tp, double pos, double total, double block_s)
{
    return tp.loops && total > 0.0 && std::isfinite(pos) && pos >= total - std::max(block_s, 0.0) - 1e-9;
}

bool EditLoopEndedAtEnd(const EditTransport& tp, double last_pos, double since_s, double total)
{
    return tp.loops && total > 0.0 && std::isfinite(last_pos) && std::isfinite(since_s) &&
           last_pos + std::max(since_s, 0.0) >= total - 0.05;
}

}  // namespace rav
