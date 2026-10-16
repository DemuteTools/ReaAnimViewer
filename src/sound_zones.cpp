// SPDX-License-Identifier: MIT
//
// See sound_zones.h. A 1:1 port of the validated mockup's zone model (maquette-zones-13-62h-v3.html) on
// SoundData: the gestures load every zone of the Sound into one working list (each zone remembering its
// variation), run the mockup's algorithm on it, then store the zones back into their variations.

#include "sound_zones.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace rav {

namespace {

constexpr double kEps = kZoneTouch;      // the mockup's EPS: two edges this close touch
constexpr double kTiny = 1e-9;
constexpr double kMoveMinPart = 0.006;   // the mockup's minLen of a part zone a move / cut drag keeps
constexpr double kCutWall = 0.01;        // RAV's cut stays this far inside its two zones (relayout)
constexpr double kSilentDb = -200.0;
constexpr double kInf = std::numeric_limits<double>::infinity();

double Clamp(double v, double lo, double hi)
{
    return std::max(lo, std::min(hi, v));
}

double Round01(double v)
{
    return std::round(v * 10.0) / 10.0;
}

double ClampDb(double v)
{
    return Clamp(Round01(v), -60.0, 24.0);
}

// 13-62i: a REAPER fade shape index (0..6), else linear.
int ShapeOk(int shape)
{
    return shape >= 0 && shape <= 6 ? shape : 0;
}

// 13-62i (Planner choice P7): the outer edges of a proposed variation (its first zone's start, its last zone's end)
// are hand edges unless its variations were detected on silence: a take, an item played whole, a take-marker slice
// keep the edges their user gave them (Pre-roll / Tail only trim what Silence detection found); its inner edges (the
// cuts between its parts) follow Pre-roll / Tail as in 13-62h.
bool ProposedOuterHand(const SoundData& d)
{
    return d.mode != DetectMode::Silence;
}

bool HasPeaks(const PeakView* p)
{
    return p && p->rate > 0.0 && !p->max_abs.empty();
}

double BinDb(const PeakView& p, long long i)
{
    if (i < 0 || i >= static_cast<long long>(p.max_abs.size())) return kSilentDb;
    const double v = static_cast<double>(p.max_abs[static_cast<size_t>(i)]);
    return v > 0.0 && std::isfinite(v) ? std::max(20.0 * std::log10(v), kSilentDb) : kSilentDb;
}

// The mockup's detect(a, b): the first and last moment at or above the Silence threshold in [a, b); nothing
// there: {a, b}.
void Detect(const SoundData& d, const PeakView& p, double a, double b, double* on, double* off)
{
    *on = a;
    *off = b;
    if (!(b > a)) return;
    const long long n = static_cast<long long>(p.max_abs.size());
    const long long lo = std::max(0LL, static_cast<long long>(std::floor(a * p.rate)));
    const long long hi = std::min(n, static_cast<long long>(std::ceil(b * p.rate)));
    long long       first = -1, last = -1;
    for (long long i = lo; i < hi; ++i)
        if (BinDb(p, i) >= d.silence.threshold_db) {
            if (first < 0) first = i;
            last = i;
        }
    if (first < 0) return;
    *on = Clamp(static_cast<double>(first) / p.rate, a, b);
    *off = Clamp(static_cast<double>(last + 1) / p.rate, *on, b);
}

double PreS(const SoundData& d, const std::string& part)
{
    const double ms = PartPrerollMs(d, part);
    return std::isfinite(ms) && ms > 0.0 ? ms / 1000.0 : 0.0;
}

double TailS(const SoundData& d, const std::string& part)
{
    const double ms = PartTailMs(d, part);
    return std::isfinite(ms) && ms > 0.0 ? ms / 1000.0 : 0.0;
}

bool SameZone(const EditZone& x, const EditZone& y)
{
    return x.a == y.a && x.b == y.b && x.part == y.part && x.slot_a == y.slot_a && x.slot_b == y.slot_b &&
           x.a_hand == y.a_hand && x.b_hand == y.b_hand && x.a_auto_cut == y.a_auto_cut && x.onset == y.onset &&
           x.snap == y.snap && x.fade_in == y.fade_in && x.fade_out == y.fade_out && x.db == y.db && x.xl == y.xl &&
           x.xr == y.xr && x.kept == y.kept && x.auto_snap == y.auto_snap && x.fade_in_shape == y.fade_in_shape &&
           x.fade_out_shape == y.fade_out_shape;
}

// What a gesture compares to tell whether it changed anything: every frame's zones, edges and on.
struct Snap {
    std::vector<std::vector<EditZone>> zones;
    std::vector<double>                start, length;
    std::vector<bool>                  on;
};

Snap Take(const SoundData& d)
{
    Snap s;
    for (const SoundFrame& f : d.frames) {
        s.zones.push_back(f.zones);
        s.start.push_back(f.start);
        s.length.push_back(f.length);
        s.on.push_back(f.on);
    }
    return s;
}

bool Same(const Snap& s, const SoundData& d)
{
    if (s.zones.size() != d.frames.size()) return false;
    for (size_t i = 0; i < d.frames.size(); ++i) {
        const SoundFrame& f = d.frames[i];
        if (f.start != s.start[i] || f.length != s.length[i] || f.on != s.on[i] || f.zones.size() != s.zones[i].size())
            return false;
        for (size_t k = 0; k < f.zones.size(); ++k)
            if (!SameZone(f.zones[k], s.zones[i][k])) return false;
    }
    return true;
}

bool ZoneLess(const EditZone& x, const EditZone& y)
{
    if (x.a != y.a) return x.a < y.a;
    return !x.IsGap() && y.IsGap();  // a Gap after a part zone starting there
}

// ---- The working list ----------------------------------------------------------------------------------------

struct WZ {
    int      frame = 0;
    int      id = 0;
    EditZone z;
};

struct Work {
    SoundData*      d = nullptr;
    const PeakView* peaks = nullptr;
    double          T = kInf;  // the file's length (inf = unknown)
    std::vector<WZ> zs;
    int             nid = 0;
    // 13-62i: the timeline the peaks, the file's length and a time given to the gesture belong to (ZoneTimeline: 0, or
    // a Takes-mode Sound's take; -1 = none). Zones of another timeline never meet these zones (no neighbour, no overlap)
    // and get no peaks nor clamp.
    int             ctl = 0;

    // Only the variations that are on take part (an off one keeps its zones untouched until it is on again).
    std::vector<bool> loaded;

    Work(SoundData& data, const PeakView* p, double file_len, int tl = -1)
        : d(&data), peaks(p), T(file_len > 0.0 ? file_len : kInf), ctl(NormTl(data, tl))
    {
        if (!(file_len > 0.0) && HasPeaks(p)) T = static_cast<double>(p->max_abs.size()) / p->rate;  // the peaks' length
        for (size_t f = 0; f < data.frames.size(); ++f) {
            loaded.push_back(data.frames[f].on);
            if (!data.frames[f].on) continue;
            for (const EditZone& z : data.frames[f].zones) zs.push_back({static_cast<int>(f), nid++, z});
        }
    }
    bool isLoaded(int f) const { return f < 0 || static_cast<size_t>(f) >= loaded.size() || loaded[static_cast<size_t>(f)]; }

    bool gap(int i) const { return zs[static_cast<size_t>(i)].z.IsGap(); }
    EditZone& Z(int i) { return zs[static_cast<size_t>(i)].z; }
    const EditZone& Z(int i) const { return zs[static_cast<size_t>(i)].z; }
    int frameOf(int i) const { return zs[static_cast<size_t>(i)].frame; }
    // 13-62i: timelines.
    int  tlOf(int i) const { return ZoneTimeline(*d, frameOf(i)); }
    bool sameTl(int x, int y) const { return tlOf(x) == tlOf(y); }
    bool onCtx(int i) const { return ctl >= 0 && tlOf(i) == ctl; }
    double Tof(int i) const { return onCtx(i) ? T : kInf; }
    const PeakView* peaksOf(int i) const { return onCtx(i) ? peaks : nullptr; }

    int byId(int id) const
    {
        for (size_t i = 0; i < zs.size(); ++i)
            if (zs[i].id == id) return static_cast<int>(i);
        return -1;
    }
    std::vector<int> ord() const  // 13-62i: by timeline, then in time order
    {
        std::vector<int> o(zs.size());
        for (size_t i = 0; i < zs.size(); ++i) o[i] = static_cast<int>(i);
        std::stable_sort(o.begin(), o.end(), [&](int x, int y) {
            if (tlOf(x) != tlOf(y)) return tlOf(x) < tlOf(y);
            return ZoneLess(Z(x), Z(y));
        });
        return o;
    }
    std::vector<int> partsOrd() const
    {
        std::vector<int> o;
        for (int i : ord())
            if (!gap(i)) o.push_back(i);
        return o;
    }
    int prevOf(int i) const
    {
        const std::vector<int> L = ord();
        for (size_t k = 0; k < L.size(); ++k)
            if (L[k] == i) return k > 0 && sameTl(L[k - 1], i) ? L[k - 1] : -1;
        return -1;
    }
    int nextOf(int i) const
    {
        const std::vector<int> L = ord();
        for (size_t k = 0; k < L.size(); ++k)
            if (L[k] == i) return k + 1 < L.size() && sameTl(L[k + 1], i) ? L[k + 1] : -1;
        return -1;
    }
    bool butted(int x, int y) const { return x >= 0 && y >= 0 && sameTl(x, y) && std::fabs(Z(x).b - Z(y).a) < kEps; }
    int zoneAt(double t) const  // on the gesture's timeline
    {
        for (int i : ord())
            if (onCtx(i) && t >= Z(i).a && t < Z(i).b) return i;
        return -1;
    }
    int gapBefore(int i) const
    {
        for (size_t g = 0; g < zs.size(); ++g)
            if (static_cast<int>(g) != i && zs[g].z.IsGap() && sameTl(static_cast<int>(g), i) && std::fabs(zs[g].z.b - Z(i).a) < kEps)
                return static_cast<int>(g);
        return -1;
    }
    int gapAfter(int i) const
    {
        for (size_t g = 0; g < zs.size(); ++g)
            if (static_cast<int>(g) != i && zs[g].z.IsGap() && sameTl(static_cast<int>(g), i) && std::fabs(zs[g].z.a - Z(i).b) < kEps)
                return static_cast<int>(g);
        return -1;
    }
    // A Gap's neighbours: the part zone of its variation ending / starting on it.
    int gapLeft(int g) const
    {
        for (int i : partsOrd())
            if (frameOf(i) == frameOf(g) && std::fabs(Z(i).b - Z(g).a) < kEps) return i;
        return -1;
    }
    int gapRight(int g) const
    {
        for (int i : partsOrd())
            if (frameOf(i) == frameOf(g) && std::fabs(Z(i).a - Z(g).b) < kEps) return i;
        return -1;
    }
    void erase(int i) { zs.erase(zs.begin() + i); }
    int push(int frame, const EditZone& z)
    {
        zs.push_back({frame, nid++, z});
        return static_cast<int>(zs.size()) - 1;
    }

    EditZone mkZone(const std::string& part, double ca, double cb) const
    {
        EditZone z;
        z.part = part;
        z.slot_a = z.a = ca;
        z.slot_b = z.b = cb;
        return z;
    }
    EditZone mkGap(double a, double b) const
    {
        EditZone g;
        g.slot_a = g.a = a;
        g.slot_b = g.b = b;
        const double w = b - a;
        const double hx = std::min(std::max(d->gap_xfade, 0.0) / 2.0, w / 2.0);
        g.xl = g.xr = std::max(hx, 0.0);
        return g;
    }

    void relayout();
    void relayoutPass();
    void sanitize();
    void store(std::map<int, ZoneRef>* where);
    int  nearestFrame(double t) const;
    std::pair<int, int> cutOut(double x0, double x1);  // ids of l / r (-1 none)
};

// 13-62h: the layout is run until it no longer changes anything (at most a few passes; one more pass when it moved
// something, to check). One pass is not stable by itself: a cut's wall reads its left zone's start, which the same
// pass then moves (an automatic start following Pre-roll), and a Gap, a zone dropped or an overlap resolved changes
// what the next pass sees. Run once, the layout laid out again (a project read back, the next gesture) moved edges
// and cuts that nobody touched.
void Work::relayout()
{
    for (int pass = 0; pass < 8; ++pass) {
        const std::vector<WZ> was = zs;
        relayoutPass();
        bool same = was.size() == zs.size();
        for (size_t i = 0; same && i < zs.size(); ++i)
            same = was[i].id == zs[i].id && was[i].frame == zs[i].frame && SameZone(was[i].z, zs[i].z);
        if (same) return;
    }
}

void Work::relayoutPass()
{
    std::vector<int> L = partsOrd();
    // RAV's cuts follow the right part's Pre-roll: between two part zones next to each other in one variation.
    // 13-62h (v5 fuzz): next in that variation, not in the Sound's time order -- another variation's zone sorted
    // between them (its edge moved by this very pass) left the cut for a second pass, so a layout was not stable
    // (a project read back laid it out differently).
    std::vector<int> prev(d->frames.size(), -1);
    for (int yi : L) {
        const int f = frameOf(yi);
        const int xi = f >= 0 && static_cast<size_t>(f) < prev.size() ? prev[static_cast<size_t>(f)] : -1;
        if (f >= 0 && static_cast<size_t>(f) < prev.size()) prev[static_cast<size_t>(f)] = yi;
        if (xi < 0) continue;
        EditZone& x = Z(xi);
        EditZone& y = Z(yi);
        if (y.a_auto_cut && y.onset >= 0.0 && std::fabs(x.slot_b - y.slot_a) < kEps) {
            const double lo = x.a + kCutWall, hi = y.slot_b - kCutWall;
            if (hi > lo) {
                const double c = Clamp(y.onset - PreS(*d, y.part), lo, hi);
                x.slot_b = c;
                y.slot_a = c;
            }
        }
    }
    for (int i : L) {
        EditZone& z = Z(i);
        if (HasPeaks(peaksOf(i))) {  // 13-62i: only the peaks of its own timeline
            double on = 0.0, off = 0.0;
            Detect(*d, *peaks, z.slot_a, z.slot_b, &on, &off);
            if (!z.a_hand) z.a = z.a_auto_cut ? z.slot_a : std::max(z.slot_a, on - PreS(*d, z.part));
            if (!z.b_hand) z.b = std::min(z.slot_b, off + TailS(*d, z.part));
        } else {  // no peaks: automatic edges stay where they are, inside their slot
            if (!z.a_hand) z.a = z.a_auto_cut ? z.slot_a : Clamp(z.a, z.slot_a, z.slot_b);
            if (!z.b_hand) z.b = std::min(z.b, z.slot_b);
        }
        if (z.b < z.a + kMinZone) {
            if (!z.b_hand) z.b = z.a + kMinZone;
            else z.a = z.b - kMinZone;
        }
    }
    // A Gap holds its two neighbours: their edges sit on it, whatever Pre-roll / Tail say.
    for (size_t gi = 0; gi < zs.size();) {
        if (!zs[gi].z.IsGap()) {
            ++gi;
            continue;
        }
        const EditZone g = zs[gi].z;
        int            l = -1, r = -1;
        // 13-62h (v5 fuzz): the zone whose edge is on the Gap first; only then one whose slot reaches it (a zone
        // before that one, its slot running over it, was taken first and the true neighbour cut away).
        for (int pass = 0; pass < 2; ++pass)
            for (int i : partsOrd()) {
                if (frameOf(i) != zs[gi].frame) continue;
                if (l < 0 && std::fabs((pass == 0 ? Z(i).b : Z(i).slot_b) - g.a) < kEps) l = i;
                if (r < 0 && std::fabs((pass == 0 ? Z(i).a : Z(i).slot_a) - g.b) < kEps) r = i;
            }
        if (l < 0 || r < 0 || l == r) {  // touches nothing on one side: empty space
            erase(static_cast<int>(gi));
            continue;
        }
        Z(l).b = g.a;
        Z(l).slot_b = g.a;
        Z(r).a = g.b;
        Z(r).slot_a = g.b;
        const double w = g.b - g.a;
        zs[gi].z.xl = Clamp(zs[gi].z.xl, 0.0, w / 2.0);
        zs[gi].z.xr = Clamp(zs[gi].z.xr, 0.0, w / 2.0);
        ++gi;
    }
    const std::vector<int> A = ord();
    for (size_t i = 0; i + 1 < A.size(); ++i) {  // never overlap (on one timeline)
        if (!sameTl(A[i], A[i + 1])) continue;
        EditZone& x = Z(A[i]);
        EditZone& y = Z(A[i + 1]);
        if (x.b > y.a + kTiny) {
            if (y.IsGap() || (!x.IsGap() && !x.b_hand)) x.b = y.a;
            else if (x.IsGap() || !y.a_hand) y.a = x.b;
            else {
                const double m = (x.b + y.a) / 2.0;
                x.b = m;
                y.a = m;
            }
        }
    }
    for (size_t i = 0; i < zs.size(); ++i) {
        zs[i].z.a = std::max(0.0, zs[i].z.a);
        zs[i].z.b = std::min(Tof(static_cast<int>(i)), zs[i].z.b);
    }
    sanitize();
}

// What the mockup leaves to chance, made sure: no zone under 4 ms, Gaps touching a part zone of their variation on
// both sides (their neighbours' edges exactly on them), no overlap, everything inside the file.
void Work::sanitize()
{
    for (int pass = 0; pass < 64; ++pass) {
        bool changed = false;
        for (size_t i = 0; i < zs.size();) {
            EditZone& z = zs[i].z;
            z.a = std::max(0.0, z.a);
            z.b = std::min(Tof(static_cast<int>(i)), z.b);
            if (!(z.b - z.a >= kMinZone - kTiny) || !std::isfinite(z.a) || !std::isfinite(z.b) ||
                (!z.IsGap() && PartIndex(*d, z.part) < 0)) {
                erase(static_cast<int>(i));
                changed = true;
                continue;
            }
            ++i;
        }
        const std::vector<int> A = ord();
        for (size_t i = 0; i + 1 < A.size(); ++i) {
            if (!sameTl(A[i], A[i + 1])) continue;
            EditZone& x = Z(A[i]);
            EditZone& y = Z(A[i + 1]);
            if (x.b > y.a + kTiny) {
                if (y.IsGap() || x.b_hand || x.IsGap()) {
                    if (x.IsGap() && !y.IsGap()) y.a = x.b;
                    else x.b = y.a;
                } else {
                    x.b = y.a;
                }
                changed = true;
            }
        }
        for (size_t gi = 0; gi < zs.size();) {
            if (!zs[gi].z.IsGap()) {
                ++gi;
                continue;
            }
            const int l = gapLeft(static_cast<int>(gi)), r = gapRight(static_cast<int>(gi));
            if (l < 0 || r < 0) {
                erase(static_cast<int>(gi));
                changed = true;
                continue;
            }
            EditZone& g = zs[gi].z;
            if (Z(l).b != g.a || Z(l).slot_b != g.a) {
                Z(l).b = g.a;
                Z(l).slot_b = g.a;
                changed = true;
            }
            if (Z(r).a != g.b || Z(r).slot_a != g.b) {
                Z(r).a = g.b;
                Z(r).slot_a = g.b;
                changed = true;
            }
            const double w = g.b - g.a;
            const double xl = Clamp(g.xl, 0.0, w / 2.0), xr = Clamp(g.xr, 0.0, w / 2.0);
            if (xl != g.xl || xr != g.xr || !std::isfinite(g.xl) || !std::isfinite(g.xr)) {
                g.xl = std::isfinite(xl) ? xl : 0.0;
                g.xr = std::isfinite(xr) ? xr : 0.0;
            }
            g.slot_a = g.a;
            g.slot_b = g.b;
            ++gi;
        }
        // 13-62h review (B3): the Gap crossfades reaching into a part zone (its left Gap's xl, its right Gap's xr)
        // together never exceed its length: at most two voices ever overlap, no fade is ever rescaled. Both cut by
        // the same factor (a gesture that sets one, SetGapXfade / MakeGap, gives it only the room left first).
        for (size_t i = 0; i < zs.size(); ++i) {
            if (zs[i].z.IsGap()) continue;
            const int    lg = gapBefore(static_cast<int>(i)), rg = gapAfter(static_cast<int>(i));
            const double len = zs[i].z.b - zs[i].z.a;
            double*      xl = lg >= 0 ? &Z(lg).xl : nullptr;
            double*      xr = rg >= 0 ? &Z(rg).xr : nullptr;
            const double sum = (xl ? *xl : 0.0) + (xr ? *xr : 0.0);
            if (sum > len + kTiny && sum > 0.0) {
                const double k = std::max(len, 0.0) / sum;
                if (xl) *xl *= k;
                if (xr) *xr *= k;
                changed = true;
            }
        }
        // A part zone's slot always holds it; its snap inside it.
        for (WZ& w : zs) {
            EditZone& z = w.z;
            if (z.IsGap()) continue;
            if (z.slot_a > z.a) z.slot_a = z.a;
            if (z.slot_b < z.b) z.slot_b = z.b;
            if (z.snap && !(*z.snap >= z.a && *z.snap <= z.b)) z.snap = Clamp(*z.snap, z.a, z.b);
            if (z.fade_in && !(*z.fade_in >= 0.0)) z.fade_in = 0.0;
            if (z.fade_out && !(*z.fade_out >= 0.0)) z.fade_out = 0.0;
        }
        if (!changed) return;
    }
    // Last resort (never seen): clip every overlap and drop what is too short.
    const std::vector<int> A = ord();
    for (size_t i = 0; i + 1 < A.size(); ++i)
        if (sameTl(A[i], A[i + 1]) && Z(A[i]).b > Z(A[i + 1]).a) Z(A[i]).b = Z(A[i + 1]).a;
    for (size_t i = zs.size(); i-- > 0;)
        if (zs[i].z.b - zs[i].z.a < kMinZone - kTiny || (zs[i].z.IsGap() && (gapLeft(static_cast<int>(i)) < 0 || gapRight(static_cast<int>(i)) < 0)))
            erase(static_cast<int>(i));
}

int Work::nearestFrame(double t) const
{
    if (d->mode == DetectMode::Takes)  // 13-62i: a take's timeline is that take's variation
        return ctl >= 0 && static_cast<size_t>(ctl) < d->frames.size() && d->frames[static_cast<size_t>(ctl)].on ? ctl : -1;
    int    best = -1;
    double bd = kInf;
    for (const WZ& w : zs) {
        const double dist = t < w.z.a ? w.z.a - t : (t >= w.z.b ? t - w.z.b : 0.0);
        if (dist < bd) {
            bd = dist;
            best = w.frame;
        }
    }
    if (best >= 0) return best;
    for (size_t f = 0; f < d->frames.size(); ++f) {
        const SoundFrame& fr = d->frames[f];
        if (!fr.on) continue;
        const double      dist = t < fr.start ? fr.start - t : (t >= fr.start + fr.length ? t - fr.start - fr.length : 0.0);
        if (dist < bd) {
            bd = dist;
            best = static_cast<int>(f);
        }
    }
    return best;
}

std::pair<int, int> Work::cutOut(double x0, double x1)
{
    int lid = -1, rid = -1;
    std::vector<int> ids;
    for (const WZ& w : zs) ids.push_back(w.id);
    for (int id : ids) {
        const int i = byId(id);
        if (i < 0 || !onCtx(i)) continue;  // 13-62i: only on the gesture's timeline
        EditZone z = Z(i);
        if (z.b <= x0 + kEps || z.a >= x1 - kEps) continue;
        const int frame = frameOf(i);
        if (z.IsGap()) {  // Antho 2026-10-14: a zone made inside a Gap splits it; only a Gap wholly covered goes
            erase(i);
            if (z.a >= x0 - kEps && z.b <= x1 + kEps) continue;
            std::vector<EditZone> keep;
            if (z.a < x0) {
                EditZone g = mkGap(z.a, x0);
                g.xl = z.xl;
                keep.push_back(g);
            }
            if (z.b > x1) {
                EditZone g = mkGap(x1, z.b);
                g.xr = z.xr;
                keep.push_back(g);
            }
            for (EditZone& g : keep)
                if (g.b - g.a >= kMinZone) {
                    const double w = g.b - g.a;
                    g.xl = std::min(g.xl, w / 2.0);
                    g.xr = std::min(g.xr, w / 2.0);
                    push(frame, g);
                }
            continue;
        }
        if (z.a >= x0 - kEps && z.b <= x1 + kEps) {
            erase(i);
            continue;
        }
        if (z.a < x0 && z.b > x1) {
            EditZone r = mkZone(z.part, x1, z.slot_b);
            r.db = z.db;
            r.a = x1;
            r.b = z.b;
            r.a_hand = true;
            r.b_hand = z.b_hand;
            r.fade_out = z.fade_out;
            r.fade_out_shape = z.fade_out_shape;
            r.snap = z.snap && *z.snap > x1 ? z.snap : std::nullopt;
            EditZone& l = Z(i);
            l.b = x0;
            l.slot_b = x0;
            l.b_hand = true;
            l.fade_out.reset();
            l.fade_out_shape = 0;
            if (l.snap && *l.snap > x0) l.snap.reset();
            lid = zs[static_cast<size_t>(i)].id;
            const int ri = push(frame, r);
            rid = zs[static_cast<size_t>(ri)].id;
            continue;
        }
        EditZone& m = Z(i);
        if (m.a < x0) {
            m.b = x0;
            m.slot_b = std::min(m.slot_b, x0);
            m.b_hand = true;
            lid = id;
        } else {
            m.a = x1;
            m.slot_a = std::max(m.slot_a, x1);
            m.a_hand = true;
            m.a_auto_cut = false;
            rid = id;
        }
    }
    if (lid < 0)
        for (size_t i = 0; i < zs.size(); ++i)
            if (!zs[i].z.IsGap() && onCtx(static_cast<int>(i)) && std::fabs(zs[i].z.b - x0) < kEps) {
                lid = zs[i].id;
                break;
            }
    if (rid < 0)
        for (size_t i = 0; i < zs.size(); ++i)
            if (!zs[i].z.IsGap() && onCtx(static_cast<int>(i)) && std::fabs(zs[i].z.a - x1) < kEps) {
                rid = zs[i].id;
                break;
            }
    return {lid, rid};
}

void Work::store(std::map<int, ZoneRef>* where)
{
    std::vector<bool> had(d->frames.size(), false);
    for (size_t f = 0; f < d->frames.size(); ++f) {
        if (!isLoaded(static_cast<int>(f))) continue;
        had[f] = !d->frames[f].zones.empty();
        d->frames[f].zones.clear();
    }
    std::vector<std::vector<std::pair<int, EditZone>>> by(d->frames.size());
    for (const WZ& w : zs)
        if (w.frame >= 0 && static_cast<size_t>(w.frame) < by.size()) by[static_cast<size_t>(w.frame)].push_back({w.id, w.z});
    for (size_t f = 0; f < by.size(); ++f) {
        if (!isLoaded(static_cast<int>(f))) continue;
        std::stable_sort(by[f].begin(), by[f].end(), [](const auto& x, const auto& y) { return ZoneLess(x.second, y.second); });
        SoundFrame& fr = d->frames[f];
        for (size_t k = 0; k < by[f].size(); ++k) {
            fr.zones.push_back(by[f][k].second);
            if (where) (*where)[by[f][k].first] = {static_cast<int>(f), static_cast<int>(k)};
        }
        if (!fr.zones.empty()) {
            double lo = kInf, hi = -kInf;
            for (const EditZone& z : fr.zones) {
                lo = std::min(lo, z.a);
                hi = std::max(hi, z.b);
            }
            fr.start = lo;
            fr.length = hi - lo;
        } else if (had[f]) {
            fr.on = false;  // a variation left with no zone plays nothing
        }
    }
}

// Ids of a selection (valid refs only, once each).
std::vector<int> IdsOf(const SoundData& d, const ZoneSel& sel)
{
    std::vector<int> out;
    for (const ZoneRef& r : sel) {
        if (r.frame < 0 || static_cast<size_t>(r.frame) >= d.frames.size()) continue;
        const SoundFrame& f = d.frames[static_cast<size_t>(r.frame)];
        if (!f.on || r.index < 0 || static_cast<size_t>(r.index) >= f.zones.size()) continue;
        int id = 0;  // the Work's ids: the zones of the on variations, in frame order
        for (int k = 0; k < r.frame; ++k)
            if (d.frames[static_cast<size_t>(k)].on) id += static_cast<int>(d.frames[static_cast<size_t>(k)].zones.size());
        id += r.index;
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    }
    return out;
}

int IdOf(const SoundData& d, const ZoneRef& r)
{
    const std::vector<int> v = IdsOf(d, {r});
    return v.empty() ? -1 : v.front();
}

// 13-62h review (R2): the zone's measured automatic snap still holds (measured for its edges, its part's Sync mode,
// the Attack threshold and its Offset as they are now).
bool AutoSnapHolds(const SoundData& d, const EditZone& z)
{
    const EditZone::AutoSnap& m = z.auto_snap;
    if (!m.set || z.IsGap() || !std::isfinite(m.t)) return false;
    const double off_ms = PartSyncOffsetMs(d, z.part);
    const double off = std::isfinite(off_ms) ? off_ms : 0.0;
    return std::fabs(m.a - z.a) <= kTiny && std::fabs(m.b - z.b) <= kTiny &&
           m.mode == static_cast<int>(PartSyncMode(d, z.part)) && m.thr == d.silence.threshold_db && m.off == off;
}

// 13-62h review (R2): with the peaks, every part zone of an on variation that has no snap of its own (and whose
// part's Sync point reads the audio: Attack / Peak) gets its automatic snap measured and stored, unless the stored
// one still holds.
void MeasureAutoSnaps(SoundData& d, const PeakView* peaks, int tl)
{
    if (!HasPeaks(peaks)) return;
    const int ctl = NormTl(d, tl);
    for (size_t v = 0; v < d.frames.size(); ++v) {
        SoundFrame& f = d.frames[v];
        if (!f.on || ctl < 0 || ZoneTimeline(d, static_cast<int>(v)) != ctl) continue;  // 13-62i: its timeline's peaks only
        for (EditZone& z : f.zones) {
            if (z.IsGap() || z.snap || AutoSnapHolds(d, z)) continue;  // only what moved (a drag stays cheap)
            const SyncMode mode = PartSyncMode(d, z.part);
            if (mode == SyncMode::SliceStart) continue;
            const double off_ms = PartSyncOffsetMs(d, z.part);
            EditZone::AutoSnap m;
            m.set = true;
            m.t = Clamp(AutoSync(d, peaks, {PartSpan{z.part, z.a, z.b - z.a}}), z.a, z.b);
            m.a = z.a;
            m.b = z.b;
            m.mode = static_cast<int>(mode);
            m.thr = d.silence.threshold_db;
            m.off = std::isfinite(off_ms) ? off_ms : 0.0;
            z.auto_snap = m;
        }
    }
}

// Ends a gesture: relayout, store, the new selection (`ids` -> refs), and whether anything changed.
void RelayoutOffFrames(SoundData& d, const PeakView* peaks, int tl);  // below

bool Finish(Work& w, const Snap& before, ZoneSel* sel, const std::vector<int>& ids)
{
    // A variation whose zones the gesture's own edit changed (before the relayout and the snaps measured, which
    // may touch other variations) is edited by hand: a re-detection (threshold, min gap) keeps it.
    std::vector<bool> touched(w.d->frames.size(), false);
    for (size_t f = 0; f < w.d->frames.size(); ++f) {
        if (!w.isLoaded(static_cast<int>(f))) continue;
        std::vector<EditZone> now;
        for (const WZ& z : w.zs)
            if (z.frame == static_cast<int>(f)) now.push_back(z.z);
        std::stable_sort(now.begin(), now.end(), ZoneLess);
        bool diff = f >= before.zones.size() || now.size() != before.zones[f].size();
        for (size_t k = 0; !diff && k < now.size(); ++k) diff = !SameZone(now[k], before.zones[f][k]);
        touched[f] = diff;
    }
    w.relayout();
    std::map<int, ZoneRef> where;
    w.store(&where);
    MeasureAutoSnaps(*w.d, w.peaks, w.ctl);
    RelayoutOffFrames(*w.d, w.peaks, w.ctl);  // a setting a gesture changed lays the off variations out too
    for (size_t f = 0; f < touched.size() && f < w.d->frames.size(); ++f)
        if (touched[f]) {
            w.d->frames[f].hand = true;
            w.d->edited = true;
        }
    if (sel) {
        sel->clear();
        for (int id : ids) {
            const auto it = where.find(id);
            if (it != where.end()) sel->push_back(it->second);
        }
    }
    return !Same(before, *w.d);
}

bool Valid(double v)
{
    return std::isfinite(v);
}

// Ends a gesture that keeps the selection: `in` (taken before the work changed anything) re-resolved into `out`.
bool FinishKeep(Work& w, const Snap& before, const ZoneSel* in, ZoneSel* out)
{
    const std::vector<int> ids = in ? IdsOf(*w.d, *in) : std::vector<int>();
    return Finish(w, before, out, ids);
}

// A direct edit's relayout (the zone count unchanged by the edit itself), the selection re-resolved.
void Relay(SoundData& d, const ZoneContext& ctx, ZoneSel* sel, const Snap* before = nullptr)
{
    Work                   w(d, ctx.peaks, ctx.file_len, ctx.tl);
    const std::vector<int> ids = sel ? IdsOf(d, *sel) : std::vector<int>();
    const Snap             now = Take(d);
    Finish(w, before ? *before : now, sel, ids);
}

}  // namespace

// ---- Basics ------------------------------------------------------------------------------------------------

bool UsesZones(const SoundData&)
{
    return true;  // 13-62i: every Sound (undivided: its implicit part; Takes mode: each take on its own source)
}

bool PlacedFromZones(const SoundData&)
{
    return true;  // 13-62i: no other model is left (a Sound read without zones is laid out first: ZonesLaidOut)
}

int ZoneTimeline(const SoundData& d, int frame)
{
    return d.mode == DetectMode::Takes ? frame : 0;
}

int NormTl(const SoundData& d, int tl)
{
    return d.mode == DetectMode::Takes ? (tl >= 0 ? tl : -1) : 0;
}

bool NeedsZoneLayout(const SoundData& d)
{
    for (const SoundFrame& f : d.frames)
        if (f.on && f.zones.empty() && f.length > 0.0) return true;
    return false;
}

const SoundData& ZonesLaidOut(const SoundData& d, SoundData* tmp)
{
    if (!NeedsZoneLayout(d) || !tmp) return d;
    *tmp = d;
    MigrateLegacyZones(*tmp);
    SanitizeZones(*tmp);
    return *tmp;
}

const EditZone* ZoneOf(const SoundData& d, const ZoneRef& r)
{
    if (r.frame < 0 || static_cast<size_t>(r.frame) >= d.frames.size()) return nullptr;
    const SoundFrame& f = d.frames[static_cast<size_t>(r.frame)];
    if (r.index < 0 || static_cast<size_t>(r.index) >= f.zones.size()) return nullptr;
    return &f.zones[static_cast<size_t>(r.index)];
}

EditZone* ZoneOf(SoundData& d, const ZoneRef& r)
{
    return const_cast<EditZone*>(ZoneOf(static_cast<const SoundData&>(d), r));
}

std::vector<ZoneRef> AllZones(const SoundData& d)
{
    std::vector<ZoneRef> out;
    for (size_t f = 0; f < d.frames.size(); ++f)
        if (d.frames[f].on)  // an off variation's zones are out of the model until it is on again
            for (size_t k = 0; k < d.frames[f].zones.size(); ++k) out.push_back({static_cast<int>(f), static_cast<int>(k)});
    std::stable_sort(out.begin(), out.end(), [&](const ZoneRef& x, const ZoneRef& y) {
        const int tx = ZoneTimeline(d, x.frame), ty = ZoneTimeline(d, y.frame);  // 13-62i: by timeline first
        if (tx != ty) return tx < ty;
        return ZoneLess(*ZoneOf(d, x), *ZoneOf(d, y));
    });
    return out;
}

std::vector<ZoneRef> AllZones(const SoundData& d, int tl)
{
    const int            ctl = NormTl(d, tl);
    std::vector<ZoneRef> out;
    for (const ZoneRef& r : AllZones(d))
        if (ctl >= 0 && ZoneTimeline(d, r.frame) == ctl) out.push_back(r);
    return out;
}

int PartZoneCount(const SoundData& d)
{
    int n = 0;
    for (const SoundFrame& f : d.frames)
        if (f.on)
            for (const EditZone& z : f.zones) n += z.IsGap() ? 0 : 1;
    return n;
}

int GapCount(const SoundData& d)
{
    int n = 0;
    for (const SoundFrame& f : d.frames)
        if (f.on)
            for (const EditZone& z : f.zones) n += z.IsGap() ? 1 : 0;
    return n;
}

std::vector<ZoneRef> ZonesOfPart(const SoundData& d, const std::string& part)
{
    std::vector<ZoneRef> out;
    for (const ZoneRef& r : AllZones(d)) {
        const EditZone* z = ZoneOf(d, r);
        if (!z->IsGap() && (part.empty() || z->part == part)) out.push_back(r);
    }
    return out;
}

namespace {

// How far into the part zone `r` its side's fade reaches (side < 0 its start, > 0 its end): a Gap's crossfade
// reaches xl into the zone after it, xr into the zone before it; any other fade its own length.
double SideReach(const SoundData& d, const ZoneRef& r, int side)
{
    const EditZone* g = ZoneOf(d, side < 0 ? GapBefore(d, r) : GapAfter(d, r));
    if (g) return side < 0 ? g->xl : g->xr;
    return ZoneFade(d, r, side).s;
}

}  // namespace

double GapReach(const SoundData& d, const ZoneRef& r, int side)
{
    const EditZone* z = ZoneOf(d, r);
    if (!z || z->IsGap()) return 0.0;
    const EditZone* g = ZoneOf(d, side < 0 ? GapBefore(d, r) : GapAfter(d, r));
    return g ? (side < 0 ? g->xl : g->xr) : 0.0;
}

std::string ZonesProblem(const SoundData& d, double file_len)
{
    const std::vector<ZoneRef> all = AllZones(d);
    for (size_t f = 0; f < d.frames.size(); ++f) {
        const SoundFrame& fr = d.frames[f];
        const std::string at = "variation " + std::to_string(f + 1) + ": ";
        for (size_t k = 0; k + 1 < fr.zones.size(); ++k)
            if (ZoneLess(fr.zones[k + 1], fr.zones[k])) return at + "zones not in time order";
        if (!fr.zones.empty()) {
            double lo = kInf, hi = -kInf;
            for (const EditZone& z : fr.zones) {
                lo = std::min(lo, z.a);
                hi = std::max(hi, z.b);
            }
            if (std::fabs(fr.start - lo) > 1e-9 || std::fabs(fr.start + fr.length - hi) > 1e-9)
                return at + "start / length are not its zones' extent";
        }
        if (fr.on && fr.zones.empty() && fr.length > 0.0) return at + "on, with no zone (the old model would place it)";
        if (!fr.zones.empty() && (!fr.cuts.empty() || !fr.owners.empty() || !fr.part_db.empty() || !fr.syncs.empty() ||
                                  !fr.hand_syncs.empty() || !fr.part_trims.empty() || fr.db != 0.0 || fr.fade_in >= 0.0 ||
                                  fr.fade_out >= 0.0))
            return at + "zones and the old model's keys (cuts / owners / part gains / frame gain / frame fades) together";
        for (const EditZone& z : fr.zones) {
            if (!std::isfinite(z.a) || !std::isfinite(z.b)) return at + "a zone edge is not finite";
            if (!(z.b - z.a >= kMinZone - 1e-9)) return at + (z.IsGap() ? "a Gap under 4 ms" : "a part zone under 4 ms");
            if (!std::isfinite(z.db) || (z.snap && !(*z.snap >= z.a - 1e-9 && *z.snap <= z.b + 1e-9)))
                return at + "a zone gain not finite, or its snap offset outside it";
            if (!z.IsGap() && !(z.slot_a <= z.a + 1e-9 && z.slot_b >= z.b - 1e-9)) return at + "a part zone outside its slot";
            if (z.a < -1e-9 || (file_len > 0.0 && z.b > file_len + 1e-9)) return at + "a zone outside the file";
            if (!z.IsGap() && PartIndex(d, z.part) < 0) return at + "a zone of a part that is not a divide value (or kWholePart)";
            if (!(z.fade_in_shape >= 0 && z.fade_in_shape <= 6 && z.fade_out_shape >= 0 && z.fade_out_shape <= 6))
                return at + "a zone fade shape out of REAPER's 0..6";
            if (z.IsGap()) {
                const double w = z.b - z.a;
                if (!(z.xl >= 0.0 && z.xr >= 0.0 && z.xl <= w / 2.0 + 1e-9 && z.xr <= w / 2.0 + 1e-9))
                    return at + "a Gap's crossfade out of [0, half its length]";
                bool l = false, r = false;
                for (const EditZone& q : fr.zones) {
                    if (q.IsGap()) continue;
                    l = l || std::fabs(q.b - z.a) < kEps;
                    r = r || std::fabs(q.a - z.b) < kEps;
                }
                if (!l || !r) return at + "a Gap that does not touch a part zone on both sides";
            }
        }
    }
    for (const ZoneRef& r : all) {  // 13-62h review (B3): what reaches into a part zone from its two sides fits in it
        const EditZone* z = ZoneOf(d, r);
        if (z->IsGap()) continue;
        if (GapReach(d, r, -1) + GapReach(d, r, 1) > z->b - z->a + 1e-9 ||
            SideReach(d, r, -1) + SideReach(d, r, 1) > z->b - z->a + 1e-9)
            return "variation " + std::to_string(r.frame + 1) + ": a part zone's crossfades / fades together longer than it";
    }
    if (UsesZones(d) && !d.zones.empty() && !all.empty()) return "Sound-level zones beside the variations' zones";
    for (size_t i = 0; i + 1 < all.size(); ++i) {
        if (ZoneTimeline(d, all[i].frame) != ZoneTimeline(d, all[i + 1].frame)) continue;  // 13-62i: takes never meet
        const EditZone* x = ZoneOf(d, all[i]);
        const EditZone* y = ZoneOf(d, all[i + 1]);
        if (x->b > y->a + 1e-9) return "two zones overlap (variations " + std::to_string(all[i].frame + 1) + " and " +
                                       std::to_string(all[i + 1].frame + 1) + ")";
    }
    return std::string();
}

// ---- Layout ----------------------------------------------------------------------------------------------------

namespace {

void RelayoutOn(SoundData& d, const PeakView* peaks, double file_len, int tl)
{
    Work w(d, peaks, file_len, tl);
    w.relayout();
    w.store(nullptr);
    MeasureAutoSnaps(d, peaks, tl);
}

}  // namespace

void RelayoutZones(SoundData& d, const PeakView* peaks, double file_len, int tl)
{
    if (!UsesZones(d)) return;
    RelayoutOn(d, peaks, file_len, tl);
    RelayoutOffFrames(d, peaks, tl);
}

namespace {

// Each off variation laid out on its own (it is out of the model: never against the others). 13-62i: with the peaks
// only when it is on their timeline.
void RelayoutOffFrames(SoundData& d, const PeakView* peaks, int tl)
{
    const int ctl = NormTl(d, tl);
    for (size_t v = 0; v < d.frames.size(); ++v) {
        if (d.frames[v].on || d.frames[v].zones.empty()) continue;
        SoundData one = d;
        for (size_t k = 0; k < one.frames.size(); ++k) one.frames[k].on = k == v;
        const bool mine = ctl >= 0 && ZoneTimeline(d, static_cast<int>(v)) == ctl;
        RelayoutOn(one, mine ? peaks : nullptr, 0.0, mine ? ctl : -1);
        d.frames[v].zones = one.frames[v].zones;
        d.frames[v].start = one.frames[v].start;
        d.frames[v].length = one.frames[v].length;
    }
}

}  // namespace

void SanitizeZones(SoundData& d)
{
    for (SoundFrame& f : d.frames) {  // 13-62i: every Sound is zoned; a bad fade shape is linear
        for (EditZone& z : f.zones) {
            if (!(z.fade_in_shape >= 0 && z.fade_in_shape <= 6)) z.fade_in_shape = 0;
            if (!(z.fade_out_shape >= 0 && z.fade_out_shape <= 6)) z.fade_out_shape = 0;
        }
    }
    for (SoundFrame& f : d.frames)  // a zoned variation keeps none of the old model's keys
        if (!f.zones.empty()) {
            f.cuts.clear();
            f.owners.clear();
            f.part_db.clear();
            f.syncs.clear();
            f.hand_syncs.clear();
            f.part_trims.clear();
            f.part_trim_key.clear();
            f.cuts_hand = f.cut_unsure = false;
            f.db = 0.0;
            f.fade_in = f.fade_out = -1.0;
            f.fade_in_shape = f.fade_out_shape = 0;
        }
    for (size_t v = 0; v < d.frames.size(); ++v)  // an on variation with none gets them (never placed by the old model)
        if (d.frames[v].on && d.frames[v].zones.empty()) ProposeZones(d, static_cast<int>(v), nullptr);
    // 13-62i: once zoned (a divided Sound's takes proposed just above too), no Sound-level zone of the old model is
    // kept; nor a parts entry of the implicit part (its settings are the Sound's).
    bool any = false;
    for (const SoundFrame& f : d.frames) any = any || !f.zones.empty();
    if (any) d.zones.clear();
    d.parts.erase(std::remove_if(d.parts.begin(), d.parts.end(), [](const PartSettings& p) { return p.value == kWholePart; }),
                  d.parts.end());
    RelayoutZones(d, nullptr);
}

void ProposeZones(SoundData& d, int v, const PeakView* peaks)
{
    if (!UsesZones(d) || v < 0 || static_cast<size_t>(v) >= d.frames.size()) return;
    SoundFrame& f = d.frames[static_cast<size_t>(v)];
    if (!(f.length > 0.0)) {
        f.on = false;  // nothing to place
        return;
    }
    const std::vector<std::string> keys = PartKeys(d);  // 13-62i: an undivided Sound's one implicit part
    const int           n = static_cast<int>(keys.size());
    bool                unsure = false;
    std::vector<double> cuts = AutoCuts(peaks, f, n, &unsure);
    std::vector<Onset>  ons;
    if (HasPeaks(peaks)) ons = FindOnsets(*peaks, f.start, f.length);
    const double fs = f.start, fe = f.start + f.length;
    const bool   outer = ProposedOuterHand(d);
    f.zones.clear();
    for (int k = 0; k < n; ++k) {
        EditZone z;
        z.part = keys[static_cast<size_t>(k)];
        z.slot_a = z.a = k == 0 ? fs : cuts[static_cast<size_t>(k - 1)];
        z.slot_b = z.b = k == n - 1 ? fe : cuts[static_cast<size_t>(k)];
        z.a_auto_cut = k > 0;
        z.a_hand = k == 0 && outer;
        z.b_hand = k == n - 1 && outer;
        z.db = f.db;  // C3: the variation's gain goes into its zones
        if (k == 0) {
            if (!ons.empty()) z.onset = ons.front().rise;
            if (f.fade_in >= 0.0) {
                z.fade_in = f.fade_in;
                z.fade_in_shape = ShapeOk(f.fade_in_shape);
            }
        } else {
            // the onset RAV cut before (AutoCuts: 5 ms before its rise)
            double best = 0.003;
            for (const Onset& o : ons)
                if (std::fabs(o.rise - 0.005 - z.slot_a) < best) {
                    best = std::fabs(o.rise - 0.005 - z.slot_a);
                    z.onset = o.rise;
                }
        }
        if (k == n - 1 && f.fade_out >= 0.0) {
            z.fade_out = f.fade_out;
            z.fade_out_shape = ShapeOk(f.fade_out_shape);
        }
        if (z.b - z.a >= kMinZone) f.zones.push_back(z);
    }
    if (f.zones.empty() && fe - fs >= kMinZone) {  // too short to divide: one zone of the first part
        EditZone z;
        z.part = keys.front();
        z.slot_a = z.a = fs;
        z.slot_b = z.b = fe;
        z.a_hand = z.b_hand = outer;
        z.db = f.db;
        f.zones.push_back(z);
    }
    if (f.zones.empty()) f.on = false;  // under 4 ms: nothing to place
    f.db = 0.0;
    f.fade_in = f.fade_out = -1.0;
    f.fade_in_shape = f.fade_out_shape = 0;
    f.cuts.clear();
    f.cuts_hand = f.cut_unsure = false;
    f.syncs.clear();
    f.part_trims.clear();
    f.part_trim_key.clear();
    f.owners.clear();
    f.part_db.clear();
}

namespace {

// A variation's zones as ProposeZones made them before the peaks were read (an even split, no onset), never
// touched since: one automatic zone per divide value in order, no Gap, nothing of their own.
bool ProposedBlind(const SoundData& d, const SoundFrame& f)
{
    const std::vector<std::string> keys = PartKeys(d);
    if (f.zones.size() != keys.size() || keys.size() < 2) return false;  // 13-62i: one part has no cut to find
    const bool outer = ProposedOuterHand(d);
    bool blind = false;
    for (size_t k = 0; k < f.zones.size(); ++k) {
        const EditZone& z = f.zones[k];
        // 13-62i: the outer edges as ProposeZones sets them (ProposedOuterHand), the inner ones automatic.
        const bool ah = k == 0 && outer, bh = k + 1 == f.zones.size() && outer;
        if (z.IsGap() || z.part != keys[k] || z.a_hand != ah || z.b_hand != bh || z.snap || z.fade_in || z.fade_out ||
            z.db != f.zones.front().db || !z.kept.empty() || (k > 0 && !z.a_auto_cut))
            return false;
        if (k > 0 && z.onset < 0.0) blind = true;
    }
    return blind;
}

}  // namespace

void EnsureZones(SoundData& d, const PeakView* peaks, double file_len, int tl)
{
    if (!UsesZones(d)) return;
    const int ctl = NormTl(d, tl);
    for (size_t v = 0; v < d.frames.size(); ++v) {
        SoundFrame&     f = d.frames[v];
        const PeakView* pk = ctl >= 0 && ZoneTimeline(d, static_cast<int>(v)) == ctl ? peaks : nullptr;  // 13-62i
        if (f.zones.empty()) {  // every variation, on or off: a zoned Sound never keeps a frame of the old model
            ProposeZones(d, static_cast<int>(v), pk);
        } else if (f.on && HasPeaks(pk) && ProposedBlind(d, f)) {  // proposed before the peaks: RAV's cuts now
            const double db = f.zones.front().db;
            f.db = db;
            f.zones.clear();
            ProposeZones(d, static_cast<int>(v), pk);
        }
    }
    RelayoutZones(d, peaks, file_len, tl);
}

namespace {

// 13-62i: an undivided Sound's variations (any mode, Takes too) read from the old model: each placed stretch of a
// variation (PlacedSpans: the frame minus its excluded Sound-level zones; a take: the whole take) becomes a zone of the
// implicit part placed as the old model placed it -- its edges by hand (Pre-roll / Tail never move them), the frame's
// gain, the frame's own fades (and shapes) on the frame's edges, the Sound's cut_fade as its own fade at an excluded
// zone's edge, the hand sync point as its own snap, else the stored sync point as its measured snap (auto_snap, or its
// own snap when it sits in a later zone, so the event still lands by it). A take keeps no sync point (P5: the zone's
// Sync point now). The old keys are cleared. False when there was nothing to migrate.
bool MigrateWholeFrames(SoundData& d)
{
    const bool                         takes = d.mode == DetectMode::Takes;
    std::vector<std::vector<EditZone>> made(d.frames.size());
    for (size_t i = 0; i < d.frames.size(); ++i) {
        SoundData tmp = d;  // as if on (an off variation keeps its zones for later)
        tmp.frames[i].on = true;
        const SoundFrame&     f = d.frames[i];
        const double          fs = f.start, fe = f.start + f.length;
        std::vector<PartSpan> spans;
        if (takes) {
            if (f.length > 0.0) spans.push_back({std::string(), fs, f.length});
        } else {
            spans = PlacedSpans(tmp, static_cast<int>(i), std::string());
        }
        const double stored = takes ? -1.0 : SyncFor(tmp, static_cast<int>(i), std::string());
        double       hand = -1.0;
        if (!takes)
            for (double h : f.hand_syncs)
                for (const PartSpan& sp : spans)
                    if (hand < 0.0 && h >= sp.start - 1e-9 && h <= sp.start + sp.length + 1e-9) hand = h;
        const double off_ms = PartSyncOffsetMs(d, kWholePart);
        for (const PartSpan& sp : spans) {
            if (sp.length < kMinZone - 1e-9) continue;
            EditZone z;
            z.part = kWholePart;
            z.a = z.slot_a = sp.start;
            z.b = z.slot_b = sp.start + sp.length;
            z.a_hand = z.b_hand = true;
            z.db = std::isfinite(f.db) ? Clamp(f.db, -60.0, 24.0) : 0.0;
            const double cut = std::isfinite(d.cut_fade) && d.cut_fade > 0.0 ? d.cut_fade : 0.0;
            if (std::fabs(z.a - fs) < 1e-9) {
                if (f.fade_in >= 0.0) {
                    z.fade_in = f.fade_in;
                    z.fade_in_shape = ShapeOk(f.fade_in_shape);
                }
            } else {
                z.fade_in = cut;  // at an excluded zone's edge
            }
            if (std::fabs(z.b - fe) < 1e-9) {
                if (f.fade_out >= 0.0) {
                    z.fade_out = f.fade_out;
                    z.fade_out_shape = ShapeOk(f.fade_out_shape);
                }
            } else {
                z.fade_out = cut;
            }
            const bool first = made[i].empty();
            if (hand >= z.a - 1e-9 && hand <= z.b + 1e-9) {
                z.snap = Clamp(hand, z.a, z.b);
                hand = -1.0;
            } else if (stored >= z.a - 1e-9 && stored <= z.b + 1e-9 && hand < 0.0) {
                const SyncMode mode = PartSyncMode(d, kWholePart);
                if (!first) {
                    z.snap = Clamp(stored, z.a, z.b);  // the event lands by it (the anchor: a zone with its own snap)
                } else if (mode != SyncMode::SliceStart) {
                    z.auto_snap.set = true;
                    z.auto_snap.t = Clamp(stored, z.a, z.b);
                    z.auto_snap.a = z.a;
                    z.auto_snap.b = z.b;
                    z.auto_snap.mode = static_cast<int>(mode);
                    z.auto_snap.thr = d.silence.threshold_db;
                    z.auto_snap.off = std::isfinite(off_ms) ? off_ms : 0.0;
                }
            }
            made[i].push_back(z);
        }
    }
    for (size_t i = 0; i < d.frames.size(); ++i) {
        SoundFrame& f = d.frames[i];
        f.zones = made[i];
        if (f.zones.empty()) {
            f.on = false;
        } else {
            f.start = f.zones.front().a;
            f.length = f.zones.back().b - f.start;
        }
        f.db = 0.0;
        f.fade_in = f.fade_out = -1.0;
        f.fade_in_shape = f.fade_out_shape = 0;
        f.cuts.clear();
        f.cuts_hand = f.cut_unsure = false;
        f.syncs.clear();
        f.hand_syncs.clear();
        f.part_db.clear();
        f.part_trims.clear();
        f.part_trim_key.clear();
        f.owners.clear();
    }
    d.zones.clear();
    return true;
}

}  // namespace

bool MigrateLegacyZones(SoundData& d)
{
    if (!UsesZones(d) || d.frames.empty()) return false;
    for (const SoundFrame& f : d.frames)
        if (!f.zones.empty()) return false;
    // 13-62i: an undivided Sound whose on variations have no zone is of the old model (a variation left with no zone
    // by a gesture is off: one with every variation off and no zone is a zoned Sound emptied, nothing to migrate).
    if (!IsDivided(d)) return NeedsZoneLayout(d) && MigrateWholeFrames(d);
    if (d.mode == DetectMode::Takes) return false;    // 13-62i: a divided Sound's takes get proposed zones (SanitizeZones)
    bool legacy = !d.zones.empty();
    for (const SoundFrame& f : d.frames)
        legacy = legacy || !f.cuts.empty() || !f.owners.empty() || !f.part_db.empty() || !f.hand_syncs.empty() ||
                 !f.part_trims.empty() || !f.syncs.empty() || f.db != 0.0;
    for (const PartSettings& p : d.parts) legacy = legacy || p.gain_db.has_value();
    if (!legacy) return false;
    std::vector<std::vector<EditZone>> made(d.frames.size());
    for (size_t i = 0; i < d.frames.size(); ++i) {
        SoundData tmp = d;  // the old model's pieces, as if on (an off variation keeps its zones for later)
        tmp.frames[i].on = true;
        const SoundFrame&           f = d.frames[i];
        const std::vector<PartSpan> pieces = PartPieces(tmp, static_cast<int>(i));
        const std::vector<PartSpan> raw = PartSpansOf(tmp, static_cast<int>(i));
        const double                fs = f.start, fe = f.start + f.length;
        for (const PartSpan& p : pieces) {
            if (p.length < kMinZone - 1e-9 || PartIndex(d, p.value_uid) < 0) continue;
            EditZone z;
            z.part = p.value_uid;
            z.a = p.start;
            z.b = p.start + p.length;
            z.slot_a = z.a;
            z.slot_b = z.b;
            const double mid = (z.a + z.b) / 2.0;
            for (const PartSpan& s : raw)
                if (mid >= s.start && mid <= s.start + s.length) {
                    z.slot_a = std::min(s.start, z.a);
                    z.slot_b = std::max(s.start + s.length, z.b);
                }
            z.db = ClampDb(FrameGainDb(f, p.value_uid) + PartGainDb(d, p.value_uid));
            if (f.fade_in >= 0.0 && std::fabs(z.a - fs) < 1e-9) {
                z.fade_in = f.fade_in;
                z.fade_in_shape = ShapeOk(f.fade_in_shape);  // 13-62i
            }
            if (f.fade_out >= 0.0 && std::fabs(z.b - fe) < 1e-9) {
                z.fade_out = f.fade_out;
                z.fade_out_shape = ShapeOk(f.fade_out_shape);
            }
            // The old model applied margins only at inner edges (cuts, excluded zones) and only in Silence mode: every
            // other edge keeps its place (by hand), so a relayout with peaks places what the old model placed.
            const bool margins = PartMarginsApply(d);
            if (!margins || std::fabs(z.a - fs) < 1e-9) z.a_hand = true;
            if (!margins || std::fabs(z.b - fe) < 1e-9) z.b_hand = true;
            for (double h : f.hand_syncs)
                if (h >= z.a && h <= z.b) {
                    std::string own;
                    if (PartAt(tmp, static_cast<int>(i), h, &own) && own == z.part) {
                        z.snap = h;
                        break;
                    }
                }
            made[i].push_back(z);
        }
    }
    for (size_t i = 0; i < d.frames.size(); ++i) {
        SoundFrame& f = d.frames[i];
        f.zones = made[i];
        std::stable_sort(f.zones.begin(), f.zones.end(), ZoneLess);
        if (f.zones.empty()) f.on = false;
        else {
            f.start = f.zones.front().a;
            f.length = f.zones.back().b - f.start;
            for (const EditZone& z : f.zones) f.length = std::max(f.length, z.b - f.start);
        }
        f.db = 0.0;
        f.fade_in = f.fade_out = -1.0;
        f.fade_in_shape = f.fade_out_shape = 0;
        f.cuts.clear();
        f.cuts_hand = f.cut_unsure = false;
        f.syncs.clear();
        f.hand_syncs.clear();
        f.part_db.clear();
        f.part_trims.clear();
        f.part_trim_key.clear();
        f.owners.clear();
    }
    d.zones.clear();
    for (PartSettings& p : d.parts)  // the part's gain is in its zones: the lane fader shows it as applied
        if (p.gain_db) {
            if (*p.gain_db != 0.0) p.fader_db = *p.gain_db;
            p.gain_db.reset();
        }
    PruneEmptyParts(d);
    return true;
}

// ---- Queries ----------------------------------------------------------------------------------------------------

ZoneRef ZoneAt(const SoundData& d, double t, int tl)
{
    for (const ZoneRef& r : AllZones(d, tl)) {
        const EditZone* z = ZoneOf(d, r);
        if (t >= z->a && t < z->b) return r;
    }
    return {};
}

ZoneRef PrevZone(const SoundData& d, const ZoneRef& r)
{
    const std::vector<ZoneRef> L = AllZones(d, ZoneTimeline(d, r.frame));  // 13-62i: on its timeline
    for (size_t k = 0; k < L.size(); ++k)
        if (L[k] == r) return k > 0 ? L[k - 1] : ZoneRef{};
    return {};
}

ZoneRef NextZone(const SoundData& d, const ZoneRef& r)
{
    const std::vector<ZoneRef> L = AllZones(d, ZoneTimeline(d, r.frame));  // 13-62i: on its timeline
    for (size_t k = 0; k < L.size(); ++k)
        if (L[k] == r) return k + 1 < L.size() ? L[k + 1] : ZoneRef{};
    return {};
}

bool Butted(const SoundData& d, const ZoneRef& x, const ZoneRef& y)
{
    const EditZone* a = ZoneOf(d, x);
    const EditZone* b = ZoneOf(d, y);
    return a && b && ZoneTimeline(d, x.frame) == ZoneTimeline(d, y.frame) && std::fabs(a->b - b->a) < kEps;
}

ZoneRef GapBefore(const SoundData& d, const ZoneRef& r)
{
    const EditZone* z = ZoneOf(d, r);
    if (!z) return {};
    for (const ZoneRef& g : AllZones(d, ZoneTimeline(d, r.frame)))
        if (g != r && ZoneOf(d, g)->IsGap() && std::fabs(ZoneOf(d, g)->b - z->a) < kEps) return g;
    return {};
}

ZoneRef GapAfter(const SoundData& d, const ZoneRef& r)
{
    const EditZone* z = ZoneOf(d, r);
    if (!z) return {};
    for (const ZoneRef& g : AllZones(d, ZoneTimeline(d, r.frame)))
        if (g != r && ZoneOf(d, g)->IsGap() && std::fabs(ZoneOf(d, g)->a - z->b) < kEps) return g;
    return {};
}

ZoneRef GapLeft(const SoundData& d, const ZoneRef& gap)
{
    const EditZone* g = ZoneOf(d, gap);
    if (!g || !g->IsGap()) return {};
    const SoundFrame& f = d.frames[static_cast<size_t>(gap.frame)];
    for (size_t k = 0; k < f.zones.size(); ++k)
        if (!f.zones[k].IsGap() && std::fabs(f.zones[k].b - g->a) < kEps) return {gap.frame, static_cast<int>(k)};
    return {};
}

ZoneRef GapRight(const SoundData& d, const ZoneRef& gap)
{
    const EditZone* g = ZoneOf(d, gap);
    if (!g || !g->IsGap()) return {};
    const SoundFrame& f = d.frames[static_cast<size_t>(gap.frame)];
    for (size_t k = 0; k < f.zones.size(); ++k)
        if (!f.zones[k].IsGap() && std::fabs(f.zones[k].a - g->b) < kEps) return {gap.frame, static_cast<int>(k)};
    return {};
}

std::vector<ZoneRef> ChainOf(const SoundData& d, const ZoneRef& r)
{
    const EditZone* z = ZoneOf(d, r);
    if (!z) return {};
    ZoneRef s = z->IsGap() ? GapLeft(d, r) : r;
    if (!ZoneOf(d, s)) return {};
    for (int guard = 0; guard < 10000; ++guard) {
        const ZoneRef g = GapBefore(d, s);
        if (!ZoneOf(d, g)) break;
        const ZoneRef l = GapLeft(d, g);
        if (!ZoneOf(d, l)) break;
        s = l;
    }
    std::vector<ZoneRef> out = {s};
    for (int guard = 0; guard < 10000; ++guard) {
        const ZoneRef g = GapAfter(d, out.back());
        if (!ZoneOf(d, g)) break;
        const ZoneRef rr = GapRight(d, g);
        if (!ZoneOf(d, rr) || std::find(out.begin(), out.end(), rr) != out.end()) break;
        out.push_back(rr);
    }
    return out;
}

std::vector<ZoneRef> ItemOf(const SoundData& d, const ZoneRef& r)
{
    const EditZone* z = ZoneOf(d, r);
    if (!z || !d.frames[static_cast<size_t>(r.frame)].on) return {};  // an off variation is out of the model
    const std::vector<ZoneRef> c = ChainOf(d, r);
    if (c.size() < 2) return {};
    std::vector<ZoneRef> out;
    for (size_t i = 0; i < c.size(); ++i) {
        out.push_back(c[i]);
        const ZoneRef g = i + 1 < c.size() ? GapAfter(d, c[i]) : ZoneRef{};
        if (ZoneOf(d, g)) out.push_back(g);
    }
    return out;
}

ZoneFadeInfo ZoneFade(const SoundData& d, const ZoneRef& r, int side)
{
    ZoneFadeInfo    out;
    const EditZone* z = ZoneOf(d, r);
    if (!z || z->IsGap()) return out;
    const bool      in = side < 0;
    // 13-62h review (B3): never more than half the zone, nor than what the other side's Gap crossfade leaves.
    const double    half = std::max(0.0, std::min((z->b - z->a) / 2.0, (z->b - z->a) - GapReach(d, r, -side)));
    const ZoneRef   g = in ? GapBefore(d, r) : GapAfter(d, r);
    if (const EditZone* gz = ZoneOf(d, g)) {
        out.s = gz->xl + gz->xr;
        out.src = FadeSrc::Gap;
        return out;
    }
    const std::optional<double>& own = in ? z->fade_in : z->fade_out;
    if (own) {
        out.s = std::min(std::max(*own, 0.0), half);
        out.src = FadeSrc::Own;
        return out;
    }
    const ZoneRef nb = in ? PrevZone(d, r) : NextZone(d, r);
    if (ZoneOf(d, nb) && (in ? Butted(d, nb, r) : Butted(d, r, nb))) {
        out.s = std::min(std::max(d.cut_fade, 0.0), half);
        out.src = FadeSrc::Cut;
        return out;
    }
    const PartSettings* p = PartSettingsOf(d, z->part);
    const std::optional<double> pf = p ? (in ? p->fade_in : p->fade_out) : std::nullopt;
    if (pf) {
        out.s = std::min(std::max(*pf, 0.0), half);
        out.src = FadeSrc::Part;
        return out;
    }
    out.s = std::min(std::max(in ? d.fade_in : d.fade_out, 0.0), half);
    out.src = FadeSrc::Sound;
    return out;
}

bool ZoneSnapStored(const SoundData& d, const ZoneRef& r)
{
    const EditZone* z = ZoneOf(d, r);
    return z && !z->IsGap() && !z->snap && PartSyncMode(d, z->part) != SyncMode::SliceStart && AutoSnapHolds(d, *z);
}

ZoneFadeInfo ZoneCutFade(const SoundData& d, const ZoneRef& r, int side)
{
    ZoneFadeInfo    out;
    const EditZone* z = ZoneOf(d, r);
    if (!z || z->IsGap()) return out;
    const double cap = std::max(0.0, std::min((z->b - z->a) / 2.0, (z->b - z->a) - GapReach(d, r, -side)));
    out.s = std::min(std::isfinite(d.cut_fade) ? std::max(d.cut_fade, 0.0) : 0.0, cap);
    out.src = FadeSrc::Cut;
    return out;
}

double ZoneEnv(const SoundData& d, const ZoneRef& r, double t)
{
    const EditZone* z = ZoneOf(d, r);
    if (!z || z->IsGap()) return 0.0;
    const EditZone* lg = ZoneOf(d, GapBefore(d, r));
    const EditZone* rg = ZoneOf(d, GapAfter(d, r));
    const double    lo = lg ? lg->b - lg->xr : z->a, hi = rg ? rg->a + rg->xl : z->b;
    if (t < lo || t >= hi) return 0.0;
    double g = 1.0;
    if (lg) {
        const double L = lg->xl + lg->xr;
        if (L > 0.0 && t < lg->b + lg->xl) g *= (t - lo) / L;
    } else {
        const double fi = ZoneFade(d, r, -1).s;
        if (fi > 0.0 && t < z->a + fi) g *= (t - z->a) / fi;
    }
    if (rg) {
        const double L = rg->xl + rg->xr;
        if (L > 0.0 && t > rg->a - rg->xr) g *= (hi - t) / L;
    } else {
        const double fo = ZoneFade(d, r, 1).s;
        if (fo > 0.0 && t > z->b - fo) g *= (z->b - t) / fo;
    }
    return Clamp(g, 0.0, 1.0);
}

ZoneSnapInfo ZoneSnap(const SoundData& d, const ZoneRef& r, const PeakView* peaks)
{
    ZoneSnapInfo    out;
    const EditZone* z = ZoneOf(d, r);
    if (!z) return out;
    if (z->snap) {
        out.t = Clamp(*z->snap, z->a, z->b);
        out.own = true;
        return out;
    }
    const SyncMode mode = PartSyncMode(d, z->part);
    if (mode != SyncMode::SliceStart && AutoSnapHolds(d, *z)) {  // 13-62h review (R2): as measured on the peaks
        out.t = Clamp(z->auto_snap.t, z->a, z->b);
        return out;
    }
    if (HasPeaks(peaks) || mode == SyncMode::SliceStart) {
        out.t = Clamp(AutoSync(d, HasPeaks(peaks) ? peaks : nullptr, {PartSpan{z->part, z->a, z->b - z->a}}), z->a, z->b);
        return out;
    }
    const double off_ms = PartSyncOffsetMs(d, z->part);
    const double off = std::isfinite(off_ms) ? off_ms / 1000.0 : 0.0;
    const double base = mode == SyncMode::Attack && z->onset >= z->a && z->onset <= z->b ? z->onset : z->a;
    out.t = Clamp(base + off, z->a, z->b);
    return out;
}

std::vector<HeardPiece> HeardPieces(const SoundData& d, double A, double B, int tl)
{
    std::vector<HeardPiece> out;
    if (!(B > A) || NormTl(d, tl) < 0) return out;
    auto inGap = [&](double t) {
        const EditZone* z = ZoneOf(d, ZoneAt(d, t, tl));
        return z && z->IsGap();
    };
    bool   have = !inGap(A);  // starting inside a jump: it resumes at its exit
    double cur = A;
    // 13-62h review (B1): a piece cut by A inside its crossfade overlaps the next one only by what is left of it;
    // the next one starts that much later in the file (the two stay aligned: same heard time, same gains).
    double     late = 0.0;
    ZoneRef    late_for;  // the piece `late` applies to (the Gap's right neighbour)
    for (const ZoneRef& r : AllZones(d, tl)) {
        const EditZone* z = ZoneOf(d, r);
        if (z->IsGap()) continue;
        const ZoneRef   lgr = GapBefore(d, r);
        const EditZone* lg = ZoneOf(d, lgr);
        const EditZone* rg = ZoneOf(d, GapAfter(d, r));
        double          a = std::max(lg ? lg->b - lg->xr : z->a, A);
        if (late > 0.0 && lg && late_for == r) a = std::max(a, lg->b - lg->xr + late);
        late = 0.0;
        const double b = std::min(rg ? rg->a + rg->xl : z->b, B);
        if (b <= a) {
            if (lg && !out.empty() && out.back().z == GapLeft(d, lgr)) out.back().xo = 0.0;  // nothing comes in
            continue;
        }
        if (have && !lg && a > cur + 1e-6) out.push_back({cur, a, 0.0, ZoneRef{}});
        HeardPiece p;
        p.a = a;
        p.b = b;
        p.xo = rg && std::fabs(b - (rg->a + rg->xl)) < 1e-9 ? rg->xl + rg->xr : 0.0;
        if (p.xo > b - a) {
            late = p.xo - (b - a);
            p.xo = b - a;
            late_for = GapRight(d, GapAfter(d, r));
        }
        p.z = r;
        out.push_back(p);
        cur = b;
        have = true;
    }
    if (!have) cur = A;
    if (B > cur + 1e-6 && !inGap(B - 1e-6)) out.push_back({cur, B, 0.0, ZoneRef{}});  // the silence up to the end
    return out;
}

bool VariationEdges(const SoundData& d, int v, double* a, double* b)
{
    if (v < 0 || static_cast<size_t>(v) >= d.frames.size()) return false;
    const SoundFrame& f = d.frames[static_cast<size_t>(v)];
    double            lo = f.start, hi = f.start + f.length;
    if (!f.zones.empty()) {
        lo = kInf;
        hi = -kInf;
        for (const EditZone& z : f.zones) {
            lo = std::min(lo, z.a);
            hi = std::max(hi, z.b);
        }
    }
    if (a) *a = lo;
    if (b) *b = hi;
    return true;
}

int NearestVariation(const SoundData& d, double t, int tl)
{
    SoundData& m = const_cast<SoundData&>(d);  // Work reads only
    Work       w(m, nullptr, 0.0, tl);
    return w.nearestFrame(t);
}

namespace {

// A zone the gestures may act on: its variation is on (an off variation is out of the zone model, its zones
// have no Work id).
const EditZone* LiveZone(const SoundData& d, const ZoneRef& r)
{
    const EditZone* z = ZoneOf(d, r);
    return z && d.frames[static_cast<size_t>(r.frame)].on ? z : nullptr;
}

EditZone* LiveZone(SoundData& d, const ZoneRef& r)
{
    return const_cast<EditZone*>(LiveZone(static_cast<const SoundData&>(d), r));
}

}  // namespace

// ---- Gestures ----------------------------------------------------------------------------------------------------

bool CutOut(SoundData& d, double x0, double x1, const ZoneContext& ctx, ZoneSel* sel)
{
    if (!UsesZones(d) || !Valid(x0) || !Valid(x1) || !(x1 > x0)) return false;
    const Snap before = Take(d);
    Work       w(d, ctx.peaks, ctx.file_len, ctx.tl);
    w.cutOut(x0, x1);
    return FinishKeep(w, before, sel, sel);
}

bool MakeGap(SoundData& d, double x0, double x1, const ZoneContext& ctx, bool* made, ZoneSel* sel)
{
    if (made) *made = false;
    if (!UsesZones(d) || !Valid(x0) || !Valid(x1) || !(x1 - x0 >= kMinZone)) return false;
    const Snap before = Take(d);
    Work       w(d, ctx.peaks, ctx.file_len, ctx.tl);
    for (size_t q = 0; q < w.zs.size(); ++q) {  // a Gap over Gaps: one Gap (13-62i: on its timeline)
        const EditZone& z = w.zs[q].z;
        if (z.IsGap() && w.onCtx(static_cast<int>(q)) && z.b >= x0 && z.a <= x1) {
            x0 = std::min(x0, z.a);
            x1 = std::max(x1, z.b);
        }
    }
    const std::pair<int, int> lr = w.cutOut(x0, x1);
    const int                 l = w.byId(lr.first), r = w.byId(lr.second);
    std::vector<int>          ids;
    if (l >= 0 && r >= 0 && w.frameOf(l) == w.frameOf(r) && std::fabs(w.Z(l).b - x0) < kEps && std::fabs(w.Z(r).a - x1) < kEps) {
        EditZone g = w.mkGap(x0, x1);
        {  // 13-62h review (B3): its crossfade gets only the room its two neighbours have left
            const int    lgl = w.gapBefore(l), rgr = w.gapAfter(r);
            const double room_r = (w.Z(l).b - w.Z(l).a) - (lgl >= 0 ? w.Z(lgl).xl : 0.0);
            const double room_l = (w.Z(r).b - w.Z(r).a) - (rgr >= 0 ? w.Z(rgr).xr : 0.0);
            g.xr = Clamp(g.xr, 0.0, std::max(room_r, 0.0));
            g.xl = Clamp(g.xl, 0.0, std::max(room_l, 0.0));
        }
        w.Z(l).b_hand = true;
        w.Z(r).a_hand = true;
        w.Z(r).a_auto_cut = false;
        const int gi = w.push(w.frameOf(l), g);
        ids.push_back(w.zs[static_cast<size_t>(gi)].id);
        if (made) *made = true;
    }
    const bool changed = Finish(w, before, sel, ids);
    if (made && *made && sel && sel->empty()) *made = false;  // relayout removed it
    return changed;
}

bool NewZone(SoundData& d, const std::string& part, double x0, double x1, const ZoneContext& ctx, ZoneSel* sel)
{
    if (!UsesZones(d) || PartIndex(d, part) < 0 || !Valid(x0) || !Valid(x1)) return false;
    if (x1 < x0) std::swap(x0, x1);
    x0 = std::max(0.0, x0);
    if (ctx.file_len > 0.0) x1 = std::min(ctx.file_len, x1);
    if (!(x1 - x0 >= kMinZone)) return false;
    const Snap before = Take(d);
    Work       w(d, ctx.peaks, ctx.file_len, ctx.tl);
    int        frame = w.nearestFrame(x0);
    if (frame < 0 && d.mode == DetectMode::Takes) return false;  // 13-62i: a take is a variation; none is made
    if (frame < 0) {  // no variation at all: a new one
        SoundFrame f;
        f.start = x0;
        f.length = x1 - x0;
        d.frames.push_back(f);
        frame = static_cast<int>(d.frames.size()) - 1;
    }
    w.cutOut(x0, x1);
    EditZone z = w.mkZone(part, x0, x1);
    z.a_hand = z.b_hand = true;
    const int zi = w.push(frame, z);
    const int id = w.zs[static_cast<size_t>(zi)].id;
    if (!d.frames[static_cast<size_t>(frame)].on) d.frames[static_cast<size_t>(frame)].on = true;
    return Finish(w, before, sel, {id});
}

bool FillGap(SoundData& d, const ZoneRef& gap, const ZoneContext& ctx, ZoneSel* sel)
{
    const EditZone* gz = LiveZone(d, gap);
    if (!UsesZones(d) || !gz || !gz->IsGap()) return false;
    const Snap before = Take(d);
    Work       w(d, ctx.peaks, ctx.file_len, ctx.tl);
    const int  g = w.byId(IdOf(d, gap));
    if (g < 0) return false;
    const int  l = w.gapLeft(g);
    const EditZone gg = w.Z(g);
    const int  lid = l >= 0 ? w.zs[static_cast<size_t>(l)].id : -1;
    w.erase(g);
    const int li = w.byId(lid);
    if (li >= 0) {
        w.Z(li).b = gg.b;
        w.Z(li).slot_b = gg.b;
        w.Z(li).b_hand = true;
    }
    return FinishKeep(w, before, sel, sel);
}

bool SplitZoneAt(SoundData& d, double t, const ZoneContext& ctx, ZoneSel* sel)
{
    if (!UsesZones(d) || !Valid(t)) return false;
    const ZoneRef   r = ZoneAt(d, t, ctx.tl);
    const EditZone* z = LiveZone(d, r);
    if (!z || z->IsGap() || t - z->a < kMinZone || z->b - t < kMinZone) return false;
    const Snap before = Take(d);
    Work       w(d, ctx.peaks, ctx.file_len, ctx.tl);
    const int  i = w.byId(IdOf(d, r));
    if (i < 0) return false;
    EditZone   rz = w.mkZone(w.Z(i).part, t, w.Z(i).slot_b);
    rz.db = w.Z(i).db;
    rz.a = t;
    rz.b = w.Z(i).b;
    rz.a_hand = true;
    rz.b_hand = w.Z(i).b_hand;
    rz.fade_out = w.Z(i).fade_out;
    rz.fade_out_shape = w.Z(i).fade_out_shape;
    rz.snap = w.Z(i).snap && *w.Z(i).snap > t ? w.Z(i).snap : std::nullopt;
    EditZone& lz = w.Z(i);
    lz.slot_b = t;
    lz.b = t;
    lz.b_hand = true;
    lz.fade_out.reset();
    lz.fade_out_shape = 0;
    if (lz.snap && *lz.snap >= t) lz.snap.reset();
    const int ri = w.push(w.frameOf(i), rz);
    return Finish(w, before, sel, {w.zs[static_cast<size_t>(ri)].id});
}

bool TrimZonesAt(SoundData& d, const ZoneSel& sel, double t, int side, const ZoneContext& ctx, ZoneSel* out)
{
    if (!UsesZones(d) || !Valid(t) || side == 0) return false;
    const Snap       before = Take(d);
    Work             w(d, ctx.peaks, ctx.file_len, ctx.tl);
    std::vector<int> L;
    for (int id : IdsOf(d, sel)) {
        const int i = w.byId(id);
        if (i >= 0 && !w.gap(i) && t > w.Z(i).a && t < w.Z(i).b) L.push_back(i);
    }
    if (L.empty()) {
        const int i = w.zoneAt(t);
        if (i >= 0 && !w.gap(i)) L.push_back(i);
    }
    for (int i : L) {
        EditZone& z = w.Z(i);
        if (side < 0) {
            if (z.b - t < kMinZone) continue;
            z.a = t;
            z.slot_a = t;
            z.a_hand = true;
            z.a_auto_cut = false;
        } else {
            if (t - z.a < kMinZone) continue;
            z.b = t;
            z.slot_b = t;
            z.b_hand = true;
        }
    }
    return FinishKeep(w, before, &sel, out);
}

bool TrimZonesTo(SoundData& d, const ZoneSel& sel, double a, double b, const ZoneContext& ctx, ZoneSel* out)
{
    if (!UsesZones(d) || !Valid(a) || !Valid(b)) return false;
    if (b < a) std::swap(a, b);
    if (b - a < kMinZone) return false;
    const Snap       before = Take(d);
    Work             w(d, ctx.peaks, ctx.file_len, ctx.tl);
    std::vector<int> L;
    for (int id : IdsOf(d, sel)) {
        const int i = w.byId(id);
        if (i >= 0 && !w.gap(i)) L.push_back(i);
    }
    if (L.empty())
        for (size_t i = 0; i < w.zs.size(); ++i)
            if (!w.zs[i].z.IsGap() && w.onCtx(static_cast<int>(i)) && w.zs[i].z.b > a && w.zs[i].z.a < b)
                L.push_back(static_cast<int>(i));
    for (int i : L) {
        EditZone&    z = w.Z(i);
        const double na = std::max(z.a, a), nb = std::min(z.b, b);
        if (nb - na < kMinZone) continue;
        if (na > z.a) {
            z.a = na;
            z.slot_a = na;
            z.a_hand = true;
            z.a_auto_cut = false;
        }
        if (nb < z.b) {
            z.b = nb;
            z.slot_b = nb;
            z.b_hand = true;
        }
    }
    return FinishKeep(w, before, &sel, out);
}

bool HealZones(SoundData& d, ZoneSel& sel, const ZoneContext& ctx, bool* mixed)
{
    if (mixed) *mixed = false;
    if (!UsesZones(d)) return false;
    const std::vector<int> ids = IdsOf(d, sel);
    if (ids.size() < 2) return false;
    const Snap       before = Take(d);
    Work             w(d, ctx.peaks, ctx.file_len, ctx.tl);
    std::vector<int> L;  // ids, time order
    for (int id : ids)
        if (w.byId(id) >= 0) L.push_back(id);
    std::stable_sort(L.begin(), L.end(), [&](int x, int y) { return ZoneLess(w.Z(w.byId(x)), w.Z(w.byId(y))); });
    std::vector<std::pair<int, int>> plan;
    int                              cur = -1;
    for (int z : L) {
        // only within one variation (V1's last zone never heals into V2's first)
        if (cur >= 0 && w.butted(w.byId(cur), w.byId(z)) && w.frameOf(w.byId(cur)) == w.frameOf(w.byId(z)))
            plan.push_back({cur, z});
        cur = z;
    }
    if (plan.empty()) return false;
    int head = -1;
    for (const auto& xy : plan) {
        const int x = w.byId(xy.first), y = w.byId(xy.second);
        if (y < 0) continue;
        const int hi = head >= 0 ? w.byId(head) : -1;
        int       into = -1;
        if (hi >= 0 && w.butted(hi, y) && w.frameOf(hi) == w.frameOf(y)) into = head;
        else if (x >= 0 && !w.gap(x)) into = xy.first;
        if (into < 0) continue;
        const int ii = w.byId(into);
        const EditZone yz = w.Z(y);
        EditZone& iz = w.Z(ii);
        if (yz.IsGap()) {
            iz.b = yz.b;
            iz.slot_b = yz.b;
        } else {
            if (yz.part != iz.part && mixed) *mixed = true;
            iz.b = yz.b;
            iz.b_hand = yz.b_hand;
            iz.slot_b = yz.slot_b;
            iz.fade_out = yz.fade_out;
            iz.fade_out_shape = yz.fade_out_shape;
        }
        w.erase(y);
        head = into;
    }
    if (head < 0) return false;
    return Finish(w, before, &sel, {head});
}

bool DeleteZones(SoundData& d, ZoneSel& sel, const ZoneContext& ctx)
{
    if (!UsesZones(d)) return false;
    const std::vector<int> ids = IdsOf(d, sel);
    if (ids.empty()) return false;
    const Snap before = Take(d);
    Work       w(d, ctx.peaks, ctx.file_len, ctx.tl);
    for (int id : ids) {  // Gaps first: given to the zone on their left
        const int g = w.byId(id);
        if (g < 0 || !w.gap(g)) continue;
        const int      l = w.gapLeft(g);
        const int      lid = l >= 0 ? w.zs[static_cast<size_t>(l)].id : -1;
        const EditZone gg = w.Z(g);
        w.erase(g);
        const int li = w.byId(lid);
        if (li >= 0) {
            w.Z(li).b = gg.b;
            w.Z(li).slot_b = gg.b;
            w.Z(li).b_hand = true;
        }
    }
    for (int id : ids) {
        const int i = w.byId(id);
        if (i >= 0 && !w.gap(i)) w.erase(i);
    }
    return Finish(w, before, &sel, {});
}

bool GiveZonesPart(SoundData& d, const ZoneSel& sel, const std::string& part, const ZoneContext& ctx, ZoneSel* out)
{
    if (!UsesZones(d) || PartIndex(d, part) < 0) return false;
    const Snap before = Take(d);
    Work       w(d, ctx.peaks, ctx.file_len, ctx.tl);
    for (int id : IdsOf(d, sel)) {
        const int i = w.byId(id);
        if (i >= 0 && !w.gap(i)) w.Z(i).part = part;
    }
    return FinishKeep(w, before, &sel, out);
}

double MoveRange(const SoundData& d, const ZoneRef& r, double delta, double file_len)
{
    const EditZone* z0 = LiveZone(d, r);
    if (!z0 || !Valid(delta)) return 0.0;
    const ZoneRef   pr = PrevZone(d, r), nr = NextZone(d, r);
    const EditZone* pv = LiveZone(d, pr);
    const EditZone* nx = LiveZone(d, nr);
    auto            minLen = [](const EditZone& z) { return z.IsGap() ? kMinZone : kMoveMinPart; };
    double          lo = -z0->a, hi = file_len > 0.0 ? file_len - z0->b : kInf;
    if (pv) lo = std::fabs(pv->b - z0->a) < kEps ? std::max(lo, pv->a + minLen(*pv) - z0->a) : std::max(lo, pv->b - z0->a);
    if (nx) hi = std::fabs(z0->b - nx->a) < kEps ? std::min(hi, nx->b - minLen(*nx) - z0->b) : std::min(hi, nx->a - z0->b);
    if (hi < lo) return 0.0;
    return Clamp(delta, std::min(lo, 0.0), std::max(hi, 0.0));
}

bool MoveZone(SoundData& d, const ZoneRef& r, double delta, const ZoneContext& ctx, ZoneSel* sel)
{
    if (!UsesZones(d) || !LiveZone(d, r)) return false;
    const double dd = MoveRange(d, r, delta, ctx.file_len);
    if (std::fabs(dd) < 1e-12) return false;
    const Snap before = Take(d);
    Work       w(d, ctx.peaks, ctx.file_len, ctx.tl);
    const int  id = IdOf(d, r);
    const int  i = w.byId(id);
    if (i < 0) return false;
    const int  pv = w.prevOf(i), nx = w.nextOf(i);
    const bool pb = pv >= 0 && w.butted(pv, i), nb = nx >= 0 && w.butted(i, nx);
    EditZone&  z = w.Z(i);
    z.a += dd;
    z.b += dd;
    if (!z.IsGap()) {
        z.a_hand = z.b_hand = true;
        z.a_auto_cut = false;
        z.slot_a = z.a;
        z.slot_b = z.b;
        if (z.snap) *z.snap += dd;
    } else {
        z.slot_a = z.a;
        z.slot_b = z.b;
    }
    const double za = z.a, zb = z.b;
    if (pb) {  // a Gap beside it stretches, still touching
        EditZone& p = w.Z(pv);
        p.b = za;
        if (!p.IsGap()) {
            p.slot_b = za;
            p.b_hand = true;
        } else {
            p.slot_b = za;
        }
    }
    if (nb) {
        EditZone& n = w.Z(nx);
        n.a = zb;
        if (!n.IsGap()) {
            n.slot_a = zb;
            n.a_hand = true;
            n.a_auto_cut = false;
        } else {
            n.slot_a = zb;
        }
    }
    return Finish(w, before, sel, {id});
}

double MoveChainRange(const SoundData& d, const ZoneRef& r, double delta, double file_len)
{
    const std::vector<ZoneRef> it = ItemOf(d, r);
    if (it.empty() || !Valid(delta)) return 0.0;
    const EditZone* f = ZoneOf(d, it.front());
    const EditZone* l = ZoneOf(d, it.back());
    const EditZone* pv = LiveZone(d, PrevZone(d, it.front()));
    const EditZone* nx = LiveZone(d, NextZone(d, it.back()));
    auto            minLen = [](const EditZone& z) { return z.IsGap() ? kMinZone : kMoveMinPart; };
    double          lo = -f->a, hi = file_len > 0.0 ? file_len - l->b : kInf;
    if (pv) lo = std::fabs(pv->b - f->a) < kEps ? std::max(lo, pv->a + minLen(*pv) - f->a) : std::max(lo, pv->b - f->a);
    if (nx) hi = std::fabs(l->b - nx->a) < kEps ? std::min(hi, nx->b - minLen(*nx) - l->b) : std::min(hi, nx->a - l->b);
    if (hi < lo) return 0.0;
    return Clamp(delta, std::min(lo, 0.0), std::max(hi, 0.0));
}

bool MoveChain(SoundData& d, const ZoneRef& r, double delta, const ZoneContext& ctx, ZoneSel* sel)
{
    if (!UsesZones(d)) return false;
    const std::vector<ZoneRef> it = ItemOf(d, r);
    if (it.empty()) return false;
    const double dd = MoveChainRange(d, r, delta, ctx.file_len);
    if (std::fabs(dd) < 1e-12) return false;
    const Snap             before = Take(d);
    Work                   w(d, ctx.peaks, ctx.file_len, ctx.tl);
    const std::vector<int> ids = IdsOf(d, it);
    const int              fi = w.byId(IdOf(d, it.front())), li = w.byId(IdOf(d, it.back()));
    if (fi < 0 || li < 0) return false;
    const int  pv = w.prevOf(fi), nx = w.nextOf(li);  // its neighbours, taken before anything moves
    const bool pb = pv >= 0 && w.butted(pv, fi), nb = nx >= 0 && w.butted(li, nx);
    for (int id : ids) {  // every member by the same amount (as MoveZone moves one zone)
        const int i = w.byId(id);
        if (i < 0) continue;
        EditZone& z = w.Z(i);
        z.a += dd;
        z.b += dd;
        z.slot_a = z.a;
        z.slot_b = z.b;
        if (!z.IsGap()) {
            z.a_hand = z.b_hand = true;
            z.a_auto_cut = false;
            if (z.snap) *z.snap += dd;
        }
    }
    const double fa = w.Z(fi).a, lb = w.Z(li).b;
    if (pb) {  // a butted neighbour's edge follows
        EditZone& p = w.Z(pv);
        p.b = p.slot_b = fa;
        if (!p.IsGap()) p.b_hand = true;
    }
    if (nb) {
        EditZone& n = w.Z(nx);
        n.a = n.slot_a = lb;
        if (!n.IsGap()) {
            n.a_hand = true;
            n.a_auto_cut = false;
        }
    }
    return Finish(w, before, sel, ids);
}

bool DragZoneEdge(SoundData& d, const ZoneRef& r, int side, double t, const ZoneContext& ctx, ZoneSel* sel)
{
    const EditZone* zz = LiveZone(d, r);
    if (!UsesZones(d) || !zz || zz->IsGap() || !Valid(t) || side == 0) return false;
    if (ZoneOf(d, side < 0 ? GapBefore(d, r) : GapAfter(d, r))) return false;  // held by a Gap
    const Snap before = Take(d);
    Work       w(d, ctx.peaks, ctx.file_len, ctx.tl);
    const int  i = w.byId(IdOf(d, r));
    if (i < 0) return false;
    const int  pv = w.prevOf(i), nx = w.nextOf(i);  // 13-62i: on its timeline
    const double T = w.T < kInf && w.onCtx(i) ? w.T : kInf;
    EditZone& z = w.Z(i);
    if (side < 0) {
        const double lo = pv >= 0 ? w.Z(pv).b : 0.0;
        const double hi = z.b - kMinZone;
        if (hi < lo) return false;
        z.a = Clamp(t, lo, hi);
        z.a_hand = true;
        z.a_auto_cut = false;
        if (z.a < z.slot_a) z.slot_a = z.a;
    } else {
        const double hi = nx >= 0 ? w.Z(nx).a : T;
        const double lo = z.a + kMinZone;
        if (hi < lo) return false;
        z.b = Clamp(t, lo, hi);
        z.b_hand = true;
        if (z.b > z.slot_b) z.slot_b = z.b;
    }
    return FinishKeep(w, before, sel, sel);
}

bool SetZoneEdge(SoundData& d, const ZoneRef& r, int side, double t, const ZoneContext& ctx, ZoneSel* sel)
{
    return DragZoneEdge(d, r, side, t, ctx, sel);
}

bool DragCut(SoundData& d, const ZoneRef& r, double t, const ZoneContext& ctx, ZoneSel* sel)
{
    const EditZone* x0 = LiveZone(d, r);
    if (!UsesZones(d) || !x0 || x0->IsGap() || !Valid(t)) return false;
    const ZoneRef   nr = NextZone(d, r);
    const EditZone* y0 = LiveZone(d, nr);
    if (!y0 || y0->IsGap() || !Butted(d, r, nr)) return false;
    const double lo = x0->a + kMoveMinPart, hi = y0->b - kMoveMinPart;
    if (hi < lo) return false;
    const double c = Clamp(t, lo, hi);
    const Snap   before = Take(d);
    Work         w(d, ctx.peaks, ctx.file_len, ctx.tl);
    EditZone&    x = w.Z(w.byId(IdOf(d, r)));
    x.b = c;
    x.slot_b = c;
    x.b_hand = true;
    EditZone& y = w.Z(w.byId(IdOf(d, nr)));
    y.a = c;
    y.slot_a = c;
    y.a_hand = true;
    y.a_auto_cut = false;
    return FinishKeep(w, before, sel, sel);
}

bool DragGapEdge(SoundData& d, const ZoneRef& gap, int side, double t, const ZoneContext& ctx, ZoneSel* sel)
{
    const EditZone* g0 = LiveZone(d, gap);
    if (!UsesZones(d) || !g0 || !g0->IsGap() || !Valid(t) || side == 0) return false;
    const Snap before = Take(d);
    Work       w(d, ctx.peaks, ctx.file_len, ctx.tl);
    const int  g = w.byId(IdOf(d, gap));
    if (g < 0) return false;
    const int  l = w.gapLeft(g), r = w.gapRight(g);
    if (side < 0) {
        const double lo = (l >= 0 ? w.Z(l).a : 0.0) + kMoveMinPart, hi = w.Z(g).b - kMinZone;
        if (hi < lo) return false;
        const double c = Clamp(t, lo, hi);
        w.Z(g).a = c;
        w.Z(g).slot_a = c;
        if (l >= 0) {
            w.Z(l).b = c;
            w.Z(l).slot_b = c;
        }
    } else {
        const double lo = w.Z(g).a + kMinZone, hi = (r >= 0 ? w.Z(r).b : w.T) - kMoveMinPart;
        if (hi < lo) return false;
        const double c = Clamp(t, lo, hi);
        w.Z(g).b = c;
        w.Z(g).slot_b = c;
        if (r >= 0) {
            w.Z(r).a = c;
            w.Z(r).slot_a = c;
        }
    }
    return FinishKeep(w, before, sel, sel);
}

bool DragGapXfade(SoundData& d, const ZoneRef& gap, int side, double t, bool both, const ZoneContext& ctx, ZoneSel* sel)
{
    const EditZone* g = LiveZone(d, gap);
    if (!UsesZones(d) || !g || !g->IsGap() || !Valid(t) || side == 0) return false;
    const double w = g->b - g->a;
    const double v = side < 0 ? Clamp(t - g->a, 0.0, w / 2.0) : Clamp(g->b - t, 0.0, w / 2.0);
    double       xl = g->xl, xr = g->xr;
    if (side < 0 || both) xl = v;
    if (side > 0 || both) xr = v;
    return SetGapXfade(d, gap, xl, xr, ctx, sel);
}

bool SetGapXfade(SoundData& d, const ZoneRef& gap, double xl, double xr, const ZoneContext& ctx, ZoneSel* sel)
{
    EditZone* g = LiveZone(d, gap);
    if (!UsesZones(d) || !g || !g->IsGap()) return false;
    const double w = g->b - g->a;
    const Snap   before = Take(d);
    // 13-62h review (B3): each side gets at most the room its zone has left (its length minus the other Gap's
    // crossfade reaching into it from its other side).
    const ZoneRef   L = GapLeft(d, gap), R = GapRight(d, gap);
    const EditZone* lz = ZoneOf(d, L);
    const EditZone* rz = ZoneOf(d, R);
    const double    room_l = rz ? std::max(0.0, (rz->b - rz->a) - GapReach(d, R, 1)) : w / 2.0;  // xl reaches into R
    const double    room_r = lz ? std::max(0.0, (lz->b - lz->a) - GapReach(d, L, -1)) : w / 2.0;  // xr into L
    if (Valid(xl) && xl >= 0.0) g->xl = Clamp(xl, 0.0, std::min(w / 2.0, room_l));
    if (Valid(xr) && xr >= 0.0) g->xr = Clamp(xr, 0.0, std::min(w / 2.0, room_r));
    Relay(d, ctx, sel, &before);
    return !Same(before, d);
}

bool SetZoneFade(SoundData& d, const ZoneRef& r, int side, double s, const ZoneContext& ctx, ZoneSel* sel)
{
    EditZone* z = LiveZone(d, r);
    if (!UsesZones(d) || !z || z->IsGap() || !Valid(s) || side == 0) return false;
    if (ZoneOf(d, side < 0 ? GapBefore(d, r) : GapAfter(d, r))) return false;  // the Gap's crossfade
    const Snap before = Take(d);
    (side < 0 ? z->fade_in : z->fade_out) = std::max(s, 0.0);
    Relay(d, ctx, sel, &before);
    return !Same(before, d);
}

bool SetZoneSnap(SoundData& d, const ZoneRef& r, double t, const ZoneContext& ctx, ZoneSel* sel)
{
    EditZone* z = LiveZone(d, r);
    if (!UsesZones(d) || !z || z->IsGap() || !Valid(t)) return false;
    const Snap before = Take(d);
    z->snap = Clamp(t, z->a, z->b);
    Relay(d, ctx, sel, &before);
    return !Same(before, d);
}

bool SetZoneDb(SoundData& d, const ZoneRef& r, double db, const ZoneContext& ctx, ZoneSel* sel)
{
    EditZone* z = LiveZone(d, r);
    if (!UsesZones(d) || !z || z->IsGap() || !Valid(db)) return false;
    const Snap before = Take(d);
    z->db = ClampDb(db);
    Relay(d, ctx, sel, &before);
    return !Same(before, d);
}

bool ShiftZonesDb(SoundData& d, const ZoneSel& sel, double delta, const std::vector<double>& base, const ZoneContext& ctx, ZoneSel* out)
{
    if (!UsesZones(d) || !Valid(delta)) return false;
    const Snap before = Take(d);
    for (size_t k = 0; k < sel.size(); ++k) {
        EditZone* z = ZoneOf(d, sel[k]);
        if (!z || z->IsGap()) continue;
        const double b0 = k < base.size() && Valid(base[k]) ? base[k] : z->db;
        z->db = ClampDb(b0 + delta);
    }
    if (out) *out = sel;
    Relay(d, ctx, out, &before);
    return !Same(before, d);
}

bool ResetZoneEdge(SoundData& d, const ZoneRef& r, int side, const ZoneContext& ctx, ZoneSel* sel)
{
    EditZone* z = LiveZone(d, r);
    if (!UsesZones(d) || !z || z->IsGap() || side == 0) return false;
    bool& h = side < 0 ? z->a_hand : z->b_hand;
    if (!h) return false;
    const Snap before = Take(d);
    h = false;
    Relay(d, ctx, sel, &before);
    return true;  // the flag itself changed
}

bool ResetZoneFade(SoundData& d, const ZoneRef& r, int side, const ZoneContext& ctx, ZoneSel* sel)
{
    EditZone* z = LiveZone(d, r);
    if (!UsesZones(d) || !z || z->IsGap() || side == 0) return false;
    std::optional<double>& f = side < 0 ? z->fade_in : z->fade_out;
    if (!f) return false;
    const Snap before = Take(d);
    f.reset();
    (side < 0 ? z->fade_in_shape : z->fade_out_shape) = 0;
    Relay(d, ctx, sel, &before);
    return true;
}

bool ResetZoneSnap(SoundData& d, const ZoneRef& r, const ZoneContext& ctx, ZoneSel* sel)
{
    EditZone* z = LiveZone(d, r);
    if (!UsesZones(d) || !z || z->IsGap() || !z->snap) return false;
    const Snap before = Take(d);
    z->snap.reset();
    Relay(d, ctx, sel, &before);
    return true;
}

double PartFaderDb(const SoundData& d, const std::string& part)
{
    if (part == kWholePart) return d.fader_db ? *d.fader_db : 0.0;  // 13-62i: the implicit part's, on the Sound
    const PartSettings* p = PartSettingsOf(d, part);
    return p && p->fader_db ? *p->fader_db : 0.0;
}

bool ShiftPartGain(SoundData& d, const std::string& part, double delta, const ZoneContext& ctx, ZoneSel* sel)
{
    if (!UsesZones(d) || PartIndex(d, part) < 0 || !Valid(delta) || std::fabs(delta) < 1e-9) return false;
    // Only as far as every zone of the part has room (none clamped), so the fader always shows what it applied.
    double up = 24.0, down = -60.0;
    for (const SoundFrame& f : d.frames)
        for (const EditZone& z : f.zones)
            if (!z.IsGap() && z.part == part) {
                up = std::min(up, 24.0 - z.db);
                down = std::max(down, -60.0 - z.db);
            }
    delta = Round01(Clamp(delta, std::min(down, 0.0), std::max(up, 0.0)));
    if (std::fabs(delta) < 1e-9) return false;
    const Snap before = Take(d);
    for (SoundFrame& f : d.frames)
        for (EditZone& z : f.zones)
            if (!z.IsGap() && z.part == part) z.db = Clamp(Round01(z.db + delta), -60.0, 24.0);
    std::optional<double>* fader = &d.fader_db;  // 13-62i: the implicit part's home
    if (part != kWholePart) {
        PartSettings* p = nullptr;
        for (PartSettings& q : d.parts)
            if (q.value == part) p = &q;
        if (!p) {
            d.parts.emplace_back();
            p = &d.parts.back();
            p->value = part;
        }
        fader = &p->fader_db;
    }
    const double was = *fader ? **fader : 0.0;
    const double now = Round01(was + delta);
    const bool   fader_changed = now != was;
    if (std::fabs(now) < 1e-9) fader->reset();
    else *fader = now;
    PruneEmptyParts(d);
    Relay(d, ctx, sel, &before);
    return fader_changed || !Same(before, d);
}

bool SetPartFader(SoundData& d, const std::string& part, double value, const ZoneContext& ctx, ZoneSel* sel)
{
    if (!Valid(value)) return false;
    return ShiftPartGain(d, part, Round01(value) - PartFaderDb(d, part), ctx, sel);
}

int ApplyPreTailChange(SoundData& d, const std::string& part, bool tail_wins, PartField field, const ZoneContext& ctx, ZoneSel* sel)
{
    if (!UsesZones(d) || (field != PartField::Preroll && field != PartField::Tail)) return 0;
    const bool pre = field == PartField::Preroll;
    auto       touched = [&](const std::string& p) {
        if (!part.empty()) return p == part;
        const PartSettings* s = PartSettingsOf(d, p);
        return !(s && (pre ? s->preroll_ms.has_value() : s->tail_ms.has_value()));
    };
    std::vector<ZoneRef> hand;
    for (const ZoneRef& r : AllZones(d)) {
        const EditZone* z = LiveZone(d, r);
        if (z->IsGap() || !touched(z->part) || !(pre ? z->a_hand : z->b_hand)) continue;
        if (ZoneOf(d, pre ? GapBefore(d, r) : GapAfter(d, r))) continue;  // held by a Gap: never moves
        hand.push_back(r);
    }
    if (tail_wins)
        for (const ZoneRef& r : hand) (pre ? LiveZone(d, r)->a_hand : LiveZone(d, r)->b_hand) = false;
    Relay(d, ctx, sel);
    return static_cast<int>(hand.size());
}

std::vector<std::vector<ZoneRef>> NormGroups(const SoundData& d, const ZoneSel& sel)
{
    std::vector<std::vector<ZoneRef>> groups;
    std::vector<ZoneRef>              seen;
    std::vector<ZoneRef>              L;
    for (const ZoneRef& r : sel) {
        const EditZone* z = LiveZone(d, r);
        if (z && !z->IsGap() && std::find(L.begin(), L.end(), r) == L.end()) L.push_back(r);
    }
    std::stable_sort(L.begin(), L.end(), [&](const ZoneRef& x, const ZoneRef& y) { return ZoneLess(*ZoneOf(d, x), *ZoneOf(d, y)); });
    for (const ZoneRef& r : L) {
        if (std::find(seen.begin(), seen.end(), r) != seen.end()) continue;
        const std::string&   p = LiveZone(d, r)->part;
        std::vector<ZoneRef> g;
        for (const ZoneRef& q : ChainOf(d, r))
            if (ZoneOf(d, q)->part == p) g.push_back(q);
        if (g.empty()) g.push_back(r);
        for (const ZoneRef& q : g) seen.push_back(q);
        groups.push_back(g);
    }
    return groups;
}

std::vector<std::vector<ZoneRef>> NormGroups(const SoundData& d, const std::string& part)
{
    return NormGroups(d, ZonesOfPart(d, part));
}

std::vector<NormRegion> NormGroupRegions(const SoundData& d, const std::vector<std::vector<ZoneRef>>& groups,
                                         NormMode sound_mode, double sound_target)
{
    std::vector<NormRegion> out;
    for (const std::vector<ZoneRef>& g : groups) {
        if (g.empty() || !ZoneOf(d, g.front())) continue;
        NormRegion r;
        r.frame = g.front().frame;
        r.part = ZoneOf(d, g.front())->part;
        for (const ZoneRef& q : g) {
            const EditZone* z = ZoneOf(d, q);
            if (z) r.spans.push_back({z->part, z->a, z->b - z->a});
        }
        r.mode = PartNormMode(d, r.part, sound_mode);
        r.target = PartNormTarget(d, r.part, sound_target);
        out.push_back(r);
    }
    return out;
}

int ApplyZoneNormalize(SoundData& d, const std::vector<std::vector<ZoneRef>>& groups, const std::vector<double>& adj_db,
                       bool reset_faders)
{
    int                   done = 0;
    std::set<std::string> touched, skipped;  // 13-62h review: a part with a silent group keeps its fader
    for (size_t i = 0; i < groups.size(); ++i) {
        const bool silent = i >= adj_db.size() || !std::isfinite(adj_db[i]) || adj_db[i] > 60.0;
        for (const ZoneRef& q : groups[i]) {
            const EditZone* z = ZoneOf(d, q);
            if (z && !z->IsGap()) (silent ? skipped : touched).insert(z->part);
        }
        if (silent) continue;
        for (const ZoneRef& q : groups[i]) {
            EditZone* z = ZoneOf(d, q);
            if (z && !z->IsGap()) z->db = ClampDb(adj_db[i]);
        }
        ++done;
    }
    if (reset_faders) {
        for (PartSettings& p : d.parts)
            if (touched.count(p.value) && !skipped.count(p.value)) p.fader_db.reset();
        if (touched.count(kWholePart) && !skipped.count(kWholePart)) d.fader_db.reset();  // 13-62i
    }
    PruneEmptyParts(d);
    return done;
}

}  // namespace rav
