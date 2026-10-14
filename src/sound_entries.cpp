// SPDX-License-Identifier: MIT
//
// See sound_entries.h. Pure: no REAPER, no file I/O (13-3, spec D1-D8).

#include "sound_entries.h"

#include "sound_record.h"  // FindGroup

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace rav {

namespace {

constexpr double kMinLen = 0.001;  // a slice / frame of 1 ms or less is empty

double NegInf()
{
    return -std::numeric_limits<double>::infinity();
}

// Splits one MARKER line into tokens; a token opening with " ' or ` runs to the same quote.
std::vector<std::string> Tokenize(const std::string& line)
{
    std::vector<std::string> out;
    size_t i = 0;
    const size_t n = line.size();
    while (i < n) {
        while (i < n && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        if (i >= n) break;
        const char q = line[i];
        if (q == '"' || q == '\'' || q == '`') {
            const size_t close = line.find(q, i + 1);
            if (close == std::string::npos) {
                out.push_back(line.substr(i + 1));
                break;
            }
            out.push_back(line.substr(i + 1, close - i - 1));
            i = close + 1;
        } else {
            size_t j = i;
            while (j < n && line[j] != ' ' && line[j] != '\t' && line[j] != '\r') ++j;
            out.push_back(line.substr(i, j - i));
            i = j;
        }
    }
    return out;
}

// Reads a number in the "C" locale (like sound_record / rule_record), whatever the process locale.
bool ParseDouble(const std::string& s, double& v)
{
    const char*                  b = s.data();
    const std::from_chars_result r = std::from_chars(b, b + s.size(), v);
    return r.ec == std::errc() && r.ptr != b;
}

bool FrameStartLess(const SoundFrame& a, const SoundFrame& b)
{
    return a.start < b.start;
}

bool ValidIndex(const SoundData& data, int i)
{
    return i >= 0 && i < static_cast<int>(data.frames.size());
}

void MarkHand(SoundData& data, int i)
{
    data.frames[static_cast<size_t>(i)].hand = true;
    data.edited = true;
}

}  // namespace

std::vector<Slice> SlicesFromRppText(const std::string& text)
{
    struct M {
        double      pos;
        std::string name;
    };
    std::vector<M> marks;
    bool has_start = false, has_end = false;
    double start_pos = 0.0, end_pos = 0.0;

    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> tok = Tokenize(line);
        if (tok.size() < 3 || tok[0] != "MARKER") continue;
        double pos = 0.0;
        if (!ParseDouble(tok[2], pos)) continue;
        const std::string name = tok.size() > 3 ? tok[3] : std::string();
        int flags = 0;
        if (tok.size() > 4) flags = std::atoi(tok[4].c_str());
        if (flags & 1) continue;  // region start or end
        if (name == "=START") {
            has_start = true;
            start_pos = pos;
        } else if (name == "=END") {
            has_end = true;
            end_pos = pos;
        } else {
            marks.push_back({pos, name});
        }
    }

    const double zero = has_start ? start_pos : 0.0;
    std::vector<M> kept;
    for (const M& m : marks) {
        if (has_start && m.pos < start_pos) continue;
        if (has_end && m.pos >= end_pos) continue;
        kept.push_back({m.pos - zero, m.name});
    }
    std::stable_sort(kept.begin(), kept.end(), [](const M& a, const M& b) { return a.pos < b.pos; });

    std::vector<Slice> out;
    for (size_t i = 0; i < kept.size(); ++i) {
        Slice s;
        s.start = kept[i].pos;
        s.name = kept[i].name;
        if (i + 1 < kept.size()) {
            s.length = kept[i + 1].pos - s.start;
        } else if (has_end) {
            s.length = (end_pos - zero) - s.start;
        } else {
            s.length = -1.0;  // to the source end
            out.push_back(s);
            continue;
        }
        if (s.length > kMinLen) out.push_back(s);
    }
    return out;
}

std::vector<Slice> SlicesFromTakeMarkers(const std::vector<TakeMarkerPos>& markers, double source_len)
{
    std::vector<TakeMarkerPos> m;
    for (const TakeMarkerPos& t : markers)
        if (t.pos >= 0.0 && t.pos < source_len) m.push_back(t);
    std::stable_sort(m.begin(), m.end(),
                     [](const TakeMarkerPos& a, const TakeMarkerPos& b) { return a.pos < b.pos; });
    std::vector<Slice> out;
    for (size_t i = 0; i < m.size(); ++i) {
        const double end = i + 1 < m.size() ? m[i + 1].pos : source_len;
        if (end - m[i].pos > kMinLen) out.push_back({m[i].pos, end - m[i].pos, m[i].name});
    }
    if (out.size() < 2) return {};
    return out;
}

std::vector<SoundFrame> SplitOnSilence(const PeakData& peaks, const SilenceParams& params, double source_len)
{
    if (peaks.rate <= 0.0 || peaks.max_abs.empty()) return {};
    const double thr = std::pow(10.0, params.threshold_db / 20.0);
    const double gap = params.min_gap_ms / 1000.0;

    struct Run {
        double a, b;
    };
    std::vector<Run> runs;
    const size_t n = peaks.max_abs.size();
    size_t i = 0;
    while (i < n) {
        if (!(static_cast<double>(peaks.max_abs[i]) >= thr)) {  // NaN counts as silence
            ++i;
            continue;
        }
        size_t j = i;
        while (j < n && static_cast<double>(peaks.max_abs[j]) >= thr) ++j;
        const Run r{static_cast<double>(i) / peaks.rate, static_cast<double>(j) / peaks.rate};
        if (!runs.empty() && r.a - runs.back().b < gap)
            runs.back().b = r.b;
        else
            runs.push_back(r);
        i = j;
    }

    const double pre = params.preroll_ms / 1000.0;
    const double tail = params.tail_ms / 1000.0;
    std::vector<double> starts(runs.size());
    for (size_t k = 0; k < runs.size(); ++k) {
        double s = runs[k].a - pre;
        if (k > 0) s = std::max(s, runs[k - 1].b);
        starts[k] = std::max(0.0, s);
    }
    std::vector<SoundFrame> out;
    for (size_t k = 0; k < runs.size(); ++k) {
        double e = runs[k].b + tail;
        if (k + 1 < runs.size()) e = std::min(e, starts[k + 1]);
        e = std::min(e, source_len);
        if (e - starts[k] > kMinLen) {
            SoundFrame f;
            f.start = starts[k];
            f.length = e - starts[k];
            out.push_back(f);
        }
    }
    return out;
}

std::vector<SoundFrame> DetectFramesAt0(const EntryFacts& facts, DetectMode mode, const SilenceParams& silence,
                                        const PeakData* peaks);

// Antho 2026-10-13: as in REAPER, the item's gain sits on what it plays: each detected variation starts
// at the source item's own volume (an item at +18.5 dB gives variations at +18.5 dB, shown on their
// boxes, drawn, heard and placed at that level). Takes: each variation is a take, at its own take's
// gain (EntryFacts::takes[k].db: the item's volume x that take's).
std::vector<SoundFrame> DetectFrames(const EntryFacts& facts, DetectMode mode, const SilenceParams& silence,
                                     const PeakData* peaks)
{
    std::vector<SoundFrame> out = DetectFramesAt0(facts, mode, silence, peaks);
    const bool per_take = mode == DetectMode::Takes && !facts.takes.empty();
    if (!per_take && std::isfinite(facts.item_db))
        for (SoundFrame& f : out) f.db = facts.item_db;
    return out;
}

std::vector<SoundFrame> DetectFramesAt0(const EntryFacts& facts, DetectMode mode, const SilenceParams& silence,
                                        const PeakData* peaks)
{
    std::vector<SoundFrame> out;
    // The item's length in source time (13-24 review: a converted Sound reads its frames in file time).
    const double rate = (std::isfinite(facts.playrate) && facts.playrate > 0.0) ? facts.playrate : 1.0;
    const double src_len = facts.item_len * rate;
    switch (mode) {
    case DetectMode::Takes:
        for (const SoundFrame& t : facts.takes) {
            SoundFrame f;
            f.start = t.start;
            f.length = t.length > 0.0 ? t.length : src_len;
            f.db = std::isfinite(t.db) ? t.db : 0.0;  // that take's gain (DetectFrames)
            out.push_back(f);
        }
        if (!out.empty()) return out;
        break;
    case DetectMode::TakeMarkers:
        for (const Slice& s : facts.slices) {
            SoundFrame f;
            f.start = s.start;
            f.length = s.length < 0.0 ? facts.source_len - s.start : s.length;
            out.push_back(f);
        }
        ClampFrames(out, facts.source_len);
        if (out.size() >= 2) return out;
        break;
    case DetectMode::Silence:
        if (peaks) {
            out = SplitOnSilence(*peaks, silence, facts.source_len);
            if (!out.empty()) return out;
        }
        break;
    case DetectMode::Whole:
        break;
    }
    const double ws = std::max(0.0, facts.start_offs);
    const double we = std::min(facts.start_offs + src_len, facts.source_len);
    if (we - ws <= kMinLen) return {};  // the item plays no part of a known source
    SoundFrame w;
    w.start = ws;
    w.length = we - ws;
    return {w};
}

void ClampFrames(std::vector<SoundFrame>& frames, double source_len)
{
    std::vector<SoundFrame> f;
    for (const SoundFrame& x : frames)
        if (x.length > kMinLen) f.push_back(x);
    std::stable_sort(f.begin(), f.end(), FrameStartLess);
    for (size_t i = 0; i < f.size(); ++i) {
        double s = std::max(0.0, f[i].start);
        double e = std::min(f[i].start + f[i].length, source_len);
        if (i + 1 < f.size()) e = std::min(e, f[i + 1].start);
        f[i].start = s;
        f[i].length = e - s;
    }
    frames.clear();
    for (const SoundFrame& x : f)
        if (x.length > kMinLen) frames.push_back(x);
}

void ResetToDetected(SoundData& data, const std::vector<SoundFrame>& detected)
{
    const std::vector<SoundFrame> old = data.frames;
    data.frames = detected;
    CarryHandMarks(data.frames, old, false);  // A7: hand sync points survive, hand cuts go
    for (SoundFrame& f : data.frames) f.owners.clear();  // 13-62: and the owners with them
    data.zones.erase(std::remove_if(data.zones.begin(), data.zones.end(), [](const SoundZone& z) { return !z.to.empty(); }),
                     data.zones.end());  // excluded zones stay, "-> value" ones go
    data.edited = false;
}

double SpanPeakDb(const PeakData& peaks, double start, double length)
{
    if (peaks.rate <= 0.0 || peaks.max_abs.empty()) return NegInf();
    // 1e-9 guards against 0.6 * 10 = 6.0000000001 reaching one peak past the frame.
    const double a = std::floor(start * peaks.rate + 1e-9);
    const double b = std::ceil((start + length) * peaks.rate - 1e-9);
    const long long n = static_cast<long long>(peaks.max_abs.size());
    const long long lo = std::max(0LL, static_cast<long long>(a));
    const long long hi = std::min(n, static_cast<long long>(b));
    float mx = 0.0f;
    for (long long i = lo; i < hi; ++i) mx = std::max(mx, peaks.max_abs[static_cast<size_t>(i)]);
    if (mx <= 0.0f) return NegInf();
    return 20.0 * std::log10(static_cast<double>(mx));
}

double FramePeakDb(const PeakData& peaks, const SoundFrame& frame)
{
    return SpanPeakDb(peaks, frame.start, frame.length);
}

DetectMode DefaultDetectMode(const EntryFacts& facts, bool has_peaks)
{
    if (facts.takes.size() >= 2) return DetectMode::Takes;
    if (facts.slices.size() >= 2) return DetectMode::TakeMarkers;
    if (has_peaks) return DetectMode::Silence;
    return DetectMode::Whole;
}

void ClampFadePair(double length, double* in, double* out)
{
    const double len = std::isfinite(length) ? std::max(length, 0.0) : 0.0;
    double a = (in && std::isfinite(*in)) ? std::max(*in, 0.0) : 0.0;
    double b = (out && std::isfinite(*out)) ? std::max(*out, 0.0) : 0.0;
    if (a + b > len) {
        const double k = a + b > 0.0 ? len / (a + b) : 0.0;
        a *= k;
        b *= k;
    }
    if (in) *in = a;
    if (out) *out = b;
}

namespace {
// A frame's own fades kept inside its length (unset sides stay unset).
void ClampOwnFades(SoundFrame& f)
{
    const bool has_in = f.fade_in >= 0.0, has_out = f.fade_out >= 0.0;
    if (!has_in && !has_out) return;
    double a = has_in ? f.fade_in : 0.0, b = has_out ? f.fade_out : 0.0;
    ClampFadePair(f.length, &a, &b);
    if (has_in) f.fade_in = a;
    if (has_out) f.fade_out = b;
}
}  // namespace

FrameFades EffectiveFades(const SoundData& data, const SoundFrame& frame)
{
    FrameFades r;
    r.own_in = frame.fade_in >= 0.0;
    r.own_out = frame.fade_out >= 0.0;
    r.in = r.own_in ? frame.fade_in : data.fade_in;
    r.out = r.own_out ? frame.fade_out : data.fade_out;
    r.in_shape = r.own_in ? frame.fade_in_shape : 0;
    r.out_shape = r.own_out ? frame.fade_out_shape : 0;
    ClampFadePair(frame.length, &r.in, &r.out);
    return r;
}

void SetFrameFades(SoundData& data, int i, double in, double out)
{
    if (!ValidIndex(data, i)) return;
    SoundFrame& f = data.frames[static_cast<size_t>(i)];
    const double len = std::max(f.length, 0.0);
    const bool   set_in = std::isfinite(in) && in >= 0.0, set_out = std::isfinite(out) && out >= 0.0;
    // The side that changed gives way to the other one (dragging a handle never moves the other).
    const double other_out = set_out ? out : std::max(data.fade_out, 0.0);
    const double other_in = set_in ? in : std::max(data.fade_in, 0.0);
    const bool   in_changed = set_in != (f.fade_in >= 0.0) || (set_in && std::fabs(in - f.fade_in) > 1e-9);
    if (set_in) in = std::min(in, len);
    if (set_out) out = std::min(out, len);
    if (set_in && set_out) {
        if (in + out > len) {
            if (in_changed) in = std::max(len - out, 0.0);
            else out = std::max(len - in, 0.0);
        }
    } else if (set_in) {
        in = std::min(in, std::max(len - std::min(other_out, len), 0.0));
    } else if (set_out) {
        out = std::min(out, std::max(len - std::min(other_in, len), 0.0));
    }
    f.fade_in = set_in ? in : -1.0;
    f.fade_out = set_out ? out : -1.0;
    if (!set_in) f.fade_in_shape = 0;
    if (!set_out) f.fade_out_shape = 0;
    MarkHand(data, i);
}

void SetDefaultFades(SoundData& data, double in, double out)
{
    data.fade_in = std::isfinite(in) ? std::max(in, 0.0) : kDefaultFadeIn;
    data.fade_out = std::isfinite(out) ? std::max(out, 0.0) : kDefaultFadeOut;
}

namespace {
// 13-62g (below, with the sections): the frame's outer pieces' edges pinned at its start / end by hand.
void PinOuterEdges(SoundData& data, int variation, bool start, bool end);
// 13-62g review: a hand edge (side -1 start / +1 end) added with its part; the part of the section holding `t`
// on that side ("" when none).
void AddHandEdge(SoundFrame& f, int side, double t, const std::string& part);
std::string SectionOwnerAt(const SoundData& data, const SoundFrame& f, double t, int side);
bool MoveCutRaw(SoundData& data, int variation, int k, double t);
void AnchorHandEdges(const SoundData& before, SoundData& data, int variation);
void CoverGivenRange(const SoundData& before, SoundData& data, int variation, double a, double b);
void TidyHandEdges(SoundData& data, int variation);

// 13-62g review: the hand edges inside the frame go; the ones on its own edges (a variation edge trimmed by hand)
// stay.
void DropInnerHandEdges(SoundFrame& f)
{
    const double fs = f.start, fe = f.start + f.length;
    auto         keep = [](std::vector<double>& v, std::vector<std::string>& parts, double at) {
        parts.resize(v.size());
        std::vector<double>      kv;
        std::vector<std::string> kp;
        for (size_t j = 0; j < v.size(); ++j)
            if (std::fabs(v[j] - at) <= 1e-9) {
                kv.push_back(v[j]);
                kp.push_back(parts[j]);
            }
        v = kv;
        parts = kp;
    };
    keep(f.edge_in, f.edge_in_part, fs);
    keep(f.edge_out, f.edge_out_part, fe);
}
}  // namespace

void TrimFrame(SoundData& data, int i, double start, double length, double source_len)
{
    if (!ValidIndex(data, i)) return;
    const size_t k = static_cast<size_t>(i);
    const double lo = k > 0 ? data.frames[k - 1].start + data.frames[k - 1].length : 0.0;
    const double hi = k + 1 < data.frames.size() ? data.frames[k + 1].start : source_len;
    const double s = std::min(std::max(start, lo), hi);
    const double e = std::min(std::max(start + length, s), hi);
    if (e - s <= kMinLen) return;  // no room: the frame stays as it was
    const double s0 = data.frames[k].start, e0 = s0 + data.frames[k].length;
    data.frames[k].start = s;
    data.frames[k].length = e - s;
    ClampOwnFades(data.frames[k]);
    MarkHand(data, i);
    data.frames[k].trimmed = true;  // 13-62g review: its range is the user's now (kept through a re-detection)
    // 13-62g: a divided variation's edge dragged by hand is its outer piece's edge moved by hand (REAPER: the item
    // edge trimmed): the piece there follows it exactly, whatever its margins.
    if (IsDivided(data) && data.mode != DetectMode::Takes) PinOuterEdges(data, i, s != s0, e != e0);
}

int AddFrame(SoundData& data, double start, double length, double source_len)
{
    double lo = 0.0, hi = source_len;
    size_t at = 0;
    for (size_t k = 0; k < data.frames.size(); ++k) {
        const SoundFrame& f = data.frames[k];
        if (f.start <= start) {
            lo = f.start + f.length;
            at = k + 1;
        } else {
            hi = std::min(hi, f.start);
            break;
        }
    }
    if (at > 0 && start < lo) return -1;  // `start` is inside an existing frame
    const double s = std::max(start, std::max(lo, 0.0));
    const double e = std::min(start + length, hi);
    if (e - s <= kMinLen) return -1;
    SoundFrame f;
    f.start = s;
    f.length = e - s;
    f.trimmed = true;  // 13-62g review: drawn by hand
    data.frames.insert(data.frames.begin() + static_cast<std::ptrdiff_t>(at), f);
    MarkHand(data, static_cast<int>(at));
    return static_cast<int>(at);
}

bool SplitFrame(SoundData& data, int i, double at)
{
    if (!ValidIndex(data, i)) return false;
    const size_t k = static_cast<size_t>(i);
    SoundFrame a = data.frames[k];
    const double end = a.start + a.length;
    if (at - a.start <= kMinLen || end - at <= kMinLen) return false;
    SoundFrame b = a;
    a.length = at - a.start;
    b.start = at;
    b.length = end - at;
    a.hand = b.hand = true;
    a.trimmed = b.trimmed = true;  // 13-62g review: the split is an edge set by hand
    // The cut gets the Sound's default fades: the left piece keeps its fade in, the right its fade out.
    a.fade_out = -1.0;
    a.fade_out_shape = 0;
    b.fade_in = -1.0;
    b.fade_in_shape = 0;
    // 13-62: a comped frame gives each half the cuts and owners on its side (the piece holding `at` is in
    // both; a cut on `at` goes). Without owners both keep every cut (RefreshParts drops the ones outside).
    if (!a.owners.empty() && a.owners.size() == a.cuts.size() + 1) {
        const std::vector<double>      cuts = a.cuts;
        const std::vector<std::string> owners = a.owners;
        a.cuts.clear();
        b.cuts.clear();
        for (double c : cuts) {
            if (c < at - kMinLen) a.cuts.push_back(c);
            if (c > at + kMinLen) b.cuts.push_back(c);
        }
        a.owners.assign(owners.begin(), owners.begin() + static_cast<std::ptrdiff_t>(a.cuts.size() + 1));
        b.owners.assign(owners.end() - static_cast<std::ptrdiff_t>(b.cuts.size() + 1), owners.end());
        a.part_trims.clear();
        b.part_trims.clear();
    }
    // 13-62g: each half keeps the hand-moved piece edges on its side; a divided Sound's halves are pinned at the
    // split (REAPER's split: both items keep the audio up to it, whatever the margins).
    // 13-62g review: a start held by [lo, hi) / an end held by (lo, hi] (as a section holds it), with its part.
    auto keep = [](std::vector<double>& v, std::vector<std::string>& parts, bool is_start, double lo, double hi) {
        parts.resize(v.size());
        std::vector<double>      kv;
        std::vector<std::string> kp;
        for (size_t j = 0; j < v.size(); ++j)
            if (is_start ? (v[j] >= lo - 1e-9 && v[j] < hi - 1e-9) : (v[j] > lo + 1e-9 && v[j] <= hi + 1e-9)) {
                kv.push_back(v[j]);
                kp.push_back(parts[j]);
            }
        v = kv;
        parts = kp;
    };
    keep(a.edge_in, a.edge_in_part, true, a.start, at);
    keep(a.edge_out, a.edge_out_part, false, a.start, at);
    keep(b.edge_in, b.edge_in_part, true, at, end);
    keep(b.edge_out, b.edge_out_part, false, at, end);
    if (IsDivided(data) && data.mode != DetectMode::Takes) {  // after any of their own (the first one held wins)
        const SoundFrame& whole = data.frames[k];
        AddHandEdge(a, 1, at, SectionOwnerAt(data, whole, at, 1));
        AddHandEdge(b, -1, at, SectionOwnerAt(data, whole, at, -1));
    }
    ClampOwnFades(a);
    ClampOwnFades(b);
    data.frames[k] = a;
    data.frames.insert(data.frames.begin() + static_cast<std::ptrdiff_t>(k) + 1, b);
    data.edited = true;
    return true;
}

void SetFrameOn(SoundData& data, int i, bool on)
{
    if (!ValidIndex(data, i)) return;
    data.frames[static_cast<size_t>(i)].on = on;
    MarkHand(data, i);
}

void SetFrameDb(SoundData& data, int i, double db)
{
    if (!ValidIndex(data, i)) return;
    SoundFrame&  f = data.frames[static_cast<size_t>(i)];
    const double delta = db - f.db;
    f.db = db;
    for (auto& kv : f.part_db) kv.second += delta;  // 13-28: its parts' own gains move with it
    MarkHand(data, i);
}

}  // namespace rav

// ---- 13-20 / 13-23 / 13-25: parts, zones, sync points ------------------------------------------

namespace rav {

namespace {

constexpr double kTimeEps = 1e-9;
constexpr double kEdgeNudge = 1e-6;  // a hand sync is kept this far inside its part's span
constexpr double kOnsetSpacing = 0.020;  // onsets >= 20 ms apart
constexpr double kOnsetWindow = 0.015;   // a rise is read over the previous 15 ms
constexpr double kOnsetRiseDb = 6.0;
constexpr double kCutLead = 0.005;       // a cut sits 5 ms before its onset's rise
constexpr double kUnsureDb = 3.0;
constexpr double kAttackBelowPeakDb = 20.0;
constexpr double kSilentDb = -200.0;
// What is read before the source's first bin: a transient at the very start of a file is an onset,
// its noise floor is not.
constexpr double kBeforeSourceDb = -90.0;

double FrameEnd(const SoundFrame& f)
{
    return f.start + f.length;
}

double BinDb(const PeakData& p, long long i)
{
    if (i < 0 || i >= static_cast<long long>(p.max_abs.size())) return kSilentDb;
    const double v = static_cast<double>(p.max_abs[static_cast<size_t>(i)]);
    return v > 0.0 && std::isfinite(v) ? std::max(20.0 * std::log10(v), kSilentDb) : kSilentDb;
}

bool HasPeaks(const PeakData* p)
{
    return p && p->rate > 0.0 && !p->max_abs.empty();
}

// The bins [lo, hi) whose start lies in [start, end).
void BinRange(const PeakData& p, double start, double end, long long* lo, long long* hi)
{
    const long long n = static_cast<long long>(p.max_abs.size());
    *lo = std::max(0LL, static_cast<long long>(std::ceil(start * p.rate - 1e-6)));
    *hi = std::min(n, static_cast<long long>(std::ceil(end * p.rate - 1e-6)));
}

bool InSpans(const std::vector<PartSpan>& spans, double t)
{
    for (const PartSpan& s : spans)
        if (t >= s.start - kTimeEps && t <= s.start + s.length + kTimeEps) return true;
    return false;
}

std::vector<double> EvenCuts(const SoundFrame& f, int parts)
{
    std::vector<double> cuts;
    for (int k = 1; k < parts; ++k) cuts.push_back(f.start + f.length * k / parts);
    return cuts;
}

// Cuts sorted, inside the frame and >= 1 ms apart from each other and the frame's edges; false when
// they cannot be.
bool SaneCuts(const SoundFrame& f, std::vector<double>& cuts)
{
    std::sort(cuts.begin(), cuts.end());
    double lo = f.start + kMinLen;
    for (size_t k = 0; k < cuts.size(); ++k) {
        const double hi = FrameEnd(f) - kMinLen * static_cast<double>(cuts.size() - k);
        if (!std::isfinite(cuts[k])) return false;
        cuts[k] = std::min(std::max(cuts[k], lo), hi);
        if (cuts[k] < lo - kTimeEps) return false;
        lo = cuts[k] + kMinLen;
    }
    return true;
}

constexpr double kMinPiece = 0.004;  // 13-62: a comped piece is at least 4 ms (AssignRange, MoveCut C4)

}  // namespace

// 13-62: the frame's owners give each piece between its cuts a divide value.
bool OwnersSane(const SoundData& data, const SoundFrame& f)
{
    if (f.owners.empty() || f.owners.size() != f.cuts.size() + 1) return false;
    for (const std::string& o : f.owners)
        if (PartIndex(data, o) < 0) return false;
    return true;
}

// 13-62: the owners the legacy rule gives the frame's pieces (piece k -> divide_values[min(k, n - 1)]).
std::vector<std::string> LegacyOwners(const SoundData& data, const SoundFrame& f)
{
    std::vector<std::string> out;
    const size_t             n = data.divide_values.size();
    if (n == 0) return out;
    for (size_t k = 0; k <= f.cuts.size(); ++k) out.push_back(data.divide_values[std::min(k, n - 1)]);
    return out;
}

namespace {

// Where a variation's pieces start and end: its edges, its cuts (divided) and the zones' edges inside it,
// sorted.
std::vector<double> PieceBounds(const SoundData& data, const SoundFrame& f)
{
    const double        fs = f.start, fe = FrameEnd(f);
    std::vector<double> b = {fs, fe};
    auto                add = [&](double t) {
        if (t > fs && t < fe) b.push_back(t);
    };
    if (IsDivided(data))
        for (double c : f.cuts) add(c);
    for (const SoundZone& z : data.zones) {
        add(z.start);
        add(z.start + z.length);
    }
    std::sort(b.begin(), b.end());
    return b;
}

}  // namespace

bool IsDivided(const SoundData& data)
{
    return !data.divide_group.empty() && !data.divide_values.empty();
}

int PartCount(const SoundData& data)
{
    return IsDivided(data) ? static_cast<int>(data.divide_values.size()) : 1;
}

int PartIndex(const SoundData& data, const std::string& value_uid)
{
    if (value_uid.empty() || !IsDivided(data)) return -1;
    for (size_t k = 0; k < data.divide_values.size(); ++k)
        if (data.divide_values[k] == value_uid) return static_cast<int>(k);
    return -1;
}

bool PartAt(const SoundData& data, int variation, double t, std::string* value)
{
    if (!ValidIndex(data, variation)) return false;
    const SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    if (!f.on || t < f.start - kTimeEps || t > FrameEnd(f) + kTimeEps) return false;
    const SoundZone* last = nullptr;
    for (const SoundZone& z : data.zones)
        if (z.length > 0.0 && t >= z.start && t < z.start + z.length) last = &z;
    if (last && last->to.empty()) return false;
    std::string owner;
    if (last && PartIndex(data, last->to) >= 0) {
        owner = last->to;
    } else if (IsDivided(data)) {
        size_t k = 0;
        while (k < f.cuts.size() && t >= f.cuts[k]) ++k;
        if (OwnersSane(data, f)) {  // 13-62: the piece's own owner
            owner = f.owners[k];
        } else {
            k = std::min(k, data.divide_values.size() - 1);
            owner = data.divide_values[k];
        }
    }
    if (value) *value = owner;
    return true;
}

std::vector<PartSpan> PartSpansOf(const SoundData& data, int variation)
{
    std::vector<PartSpan> out;
    if (!ValidIndex(data, variation)) return out;
    const SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    if (!f.on || !(f.length > 0.0)) return out;
    const std::vector<double> b = PieceBounds(data, f);
    for (size_t k = 0; k + 1 < b.size(); ++k) {
        const double a = b[k], e = b[k + 1];
        if (e - a <= kTimeEps) continue;
        std::string owner;
        if (!PartAt(data, variation, (a + e) * 0.5, &owner)) continue;
        if (!out.empty() && out.back().value_uid == owner && std::fabs(out.back().start + out.back().length - a) <= kTimeEps)
            out.back().length = e - out.back().start;
        else
            out.push_back({owner, a, e - a});
    }
    return out;
}

bool PartMarginsApply(const SoundData& data)
{
    return data.mode == DetectMode::Silence && PartCount(data) >= 2;
}

namespace {

double MarginS(double ms)
{
    return std::isfinite(ms) && ms > 0.0 ? ms / 1000.0 : 0.0;
}

// 13-62g: who owns source time `t` of frame `f` with the excluded zones ignored: the last "-> value" zone over
// it (when a divide value), else the piece's owner by the cuts. The section `t` belongs to, excluded or not.
std::string OwnerIgnoringExclusions(const SoundData& data, const SoundFrame& f, double t)
{
    const SoundZone* last = nullptr;
    for (const SoundZone& z : data.zones)
        if (z.length > 0.0 && !z.to.empty() && t >= z.start && t < z.start + z.length) last = &z;
    if (last && PartIndex(data, last->to) >= 0) return last->to;
    size_t k = 0;
    while (k < f.cuts.size() && t >= f.cuts[k]) ++k;
    if (OwnersSane(data, f)) return f.owners[k];
    return data.divide_values[std::min(k, data.divide_values.size() - 1)];
}

// 13-62g: the sections of a divided variation (exclusions ignored), as PartSpan {owner, start, length}: the
// stretches between its edges, its cuts and its "-> value" zones' edges, touching stretches of one owner
// joined. {} when combined, in Takes mode, for an off or empty frame.
std::vector<PartSpan> SectionBoundsOf(const SoundData& data, const SoundFrame& f)
{
    std::vector<PartSpan> out;
    if (!IsDivided(data) || data.mode == DetectMode::Takes) return out;
    if (!f.on || !(f.length > 0.0)) return out;
    const double        fs = f.start, fe = FrameEnd(f);
    std::vector<double> b = {fs, fe};
    auto                add = [&](double t) {
        if (t > fs && t < fe) b.push_back(t);
    };
    for (double c : f.cuts) add(c);
    for (const SoundZone& z : data.zones)
        if (!z.to.empty()) {
            add(z.start);
            add(z.start + z.length);
        }
    std::sort(b.begin(), b.end());
    for (size_t k = 0; k + 1 < b.size(); ++k) {
        const double a = b[k], e = b[k + 1];
        if (e - a <= kTimeEps) continue;
        const std::string owner = OwnerIgnoringExclusions(data, f, (a + e) * 0.5);
        if (!out.empty() && out.back().value_uid == owner && std::fabs(out.back().start + out.back().length - a) <= kTimeEps)
            out.back().length = e - out.back().start;
        else
            out.push_back({owner, a, e - a});
    }
    return out;
}

std::vector<PartSpan> SectionBounds(const SoundData& data, int variation)
{
    if (!ValidIndex(data, variation)) return {};
    return SectionBoundsOf(data, data.frames[static_cast<size_t>(variation)]);
}

// 13-62g review: a hand start (side -1) is held by section [s, e) when s <= h < e, a hand end (+1) when s < h <= e.
bool EdgeHeld(int side, double h, double s, double e)
{
    return std::isfinite(h) && (side < 0 ? (h >= s - kTimeEps && h < e - kTimeEps) : (h > s + kTimeEps && h <= e + kTimeEps));
}

// The index of the section holding hand edge `h` of side `side`, -1 when none.
int SectionHolding(const std::vector<PartSpan>& secs, int side, double h)
{
    for (size_t i = 0; i < secs.size(); ++i)
        if (EdgeHeld(side, h, secs[i].start, secs[i].start + secs[i].length)) return static_cast<int>(i);
    return -1;
}

std::vector<double>& EdgesOf(SoundFrame& f, int side)
{
    return side < 0 ? f.edge_in : f.edge_out;
}

// The parts of a side's hand edges, sized to its edges ("" = not known yet).
std::vector<std::string>& EdgePartsOf(SoundFrame& f, int side)
{
    std::vector<std::string>& p = side < 0 ? f.edge_in_part : f.edge_out_part;
    p.resize(EdgesOf(f, side).size());
    return p;
}

// Removes the hand edges of a side for which `drop(h, part)` holds; how many went.
template <class Pred>
size_t DropHandEdges(SoundFrame& f, int side, Pred drop)
{
    std::vector<double>&      e = EdgesOf(f, side);
    std::vector<std::string>& p = EdgePartsOf(f, side);
    std::vector<double>       ke;
    std::vector<std::string>  kp;
    for (size_t j = 0; j < e.size(); ++j)
        if (!drop(e[j], p[j])) {
            ke.push_back(e[j]);
            kp.push_back(p[j]);
        }
    const size_t gone = e.size() - ke.size();
    e = ke;
    p = kp;
    return gone;
}

// 13-62g (Antho 2026-10-14, "if there is a cut, it is a section of its own"): each section's automatic piece,
// one entry per section (start == end: nothing placed). Its own sound = its first to last bin at or above the
// threshold, over the whole section (an excluded stretch inside it counts: excluding never moves the edges,
// it only takes its own stretch away). start = sound start - its part's pre-roll, end = sound end + its part's
// tail, never past the section's edges (a cut is a wall both ways: no margin crosses it into a neighbour,
// which 13-58 allowed). The variation's own edges are walls like any other: the margins apply there too (the
// frame is detected with the largest margin any part has, DetectionSilence). A section with no loud bin plays
// nothing when its part sounds elsewhere in the variation, else it plays whole (a part quiet all along).
std::vector<PartSpan> MeasureSections(const SoundData& data, int variation, const PeakData& peaks)
{
    std::vector<PartSpan> secs = SectionBounds(data, variation);
    const double          thr = data.silence.threshold_db;
    const size_t          n = secs.size();
    std::vector<double>   a(n, 0.0), b(n, 0.0);
    std::vector<bool>     loud(n, false);
    std::set<std::string> sounding;
    for (size_t i = 0; i < n; ++i) {
        const PartSpan& s = secs[i];
        long long       lo = 0, hi = 0, first = -1, last = -1;
        BinRange(peaks, s.start, s.start + s.length, &lo, &hi);
        for (long long k = lo; k < hi; ++k)
            if (BinDb(peaks, k) >= thr) {
                if (first < 0) first = k;
                last = k;
            }
        if (first < 0) continue;
        loud[i] = true;
        sounding.insert(s.value_uid);
        a[i] = std::max(static_cast<double>(first) / peaks.rate, s.start);
        b[i] = std::max(std::min(static_cast<double>(last + 1) / peaks.rate, s.start + s.length), a[i]);
    }
    std::vector<PartSpan> out = secs;
    for (size_t i = 0; i < n; ++i) {
        const double ss = secs[i].start, se = secs[i].start + secs[i].length;
        if (loud[i]) {
            const double st = std::min(std::max(a[i] - MarginS(PartPrerollMs(data, secs[i].value_uid)), ss), a[i]);
            const double en = std::max(std::min(b[i] + MarginS(PartTailMs(data, secs[i].value_uid)), se), b[i]);
            out[i].start = st;
            out[i].length = en - st;
        } else if (sounding.count(secs[i].value_uid) != 0) {
            out[i].length = 0.0;  // its part sounds elsewhere here: nothing placed
        }
    }
    return out;
}

// 13-62g: the stored trims fit the sections (one start / end pair each, inside it, start <= end).
bool TrimsFit(const SoundData& data, int variation, const std::vector<PartSpan>& secs)
{
    const std::vector<double>& t = data.frames[static_cast<size_t>(variation)].part_trims;
    if (t.size() != secs.size() * 2) return false;
    for (size_t i = 0; i < secs.size(); ++i) {
        const double s = t[2 * i], e = t[2 * i + 1];
        if (!std::isfinite(s) || !std::isfinite(e) || e < s - kTimeEps || s < secs[i].start - kTimeEps ||
            e > secs[i].start + secs[i].length + kTimeEps)
            return false;
    }
    return true;
}

// 13-62g: the hand edge of side `side` that section [s, e] of part `owner` holds (a start in [s, e), an end in
// (s, e]); NaN when none. 13-62g review: only an edge of that part (or one whose part is not known yet) acts there.
double HandEdgeIn(const SoundFrame& f, int side, double s, double e, const std::string& owner)
{
    const std::vector<double>&      edges = side < 0 ? f.edge_in : f.edge_out;
    const std::vector<std::string>& parts = side < 0 ? f.edge_in_part : f.edge_out_part;
    for (size_t j = 0; j < edges.size(); ++j) {
        const std::string part = j < parts.size() ? parts[j] : std::string();
        if (EdgeHeld(side, edges[j], s, e) && (part.empty() || part == owner)) return edges[j];
    }
    return std::numeric_limits<double>::quiet_NaN();
}

}  // namespace

std::string PartTrimKey(const SoundData& data, int variation, const FileVersion& file)
{
    // The sections to the microsecond with their owners (FNV-1a): a moved cut, a "-> value" zone, another divide
    // makes another key, even when written by a RAV that does not know the trims (it keeps them as read).
    // 13-62g: the sections, not the spans: an excluded zone never changes what is measured.
    auto us = [](double t) { return std::isfinite(t) ? static_cast<long long>(std::llround(t * 1e6)) : 0LL; };
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](const std::string& text) {
        for (unsigned char c : text) {
            h ^= c;
            h *= 1099511628211ULL;
        }
    };
    if (ValidIndex(data, variation))
        for (const PartSpan& sp : SectionBounds(data, variation))
            mix(std::to_string(us(sp.start)) + ":" + std::to_string(us(sp.start + sp.length)) + ":" + sp.value_uid + ";");
    char buf[160];
    // The settings in millionths (dB, ms): stable through the record's shortest round-trip numbers.
    // Led by the margin rule's version: 13-58 "r2" (tails served before pre-rolls), 13-62g "r3" (one piece per
    // section, the margins never cross a cut, applied at the variation's edges too). Trims measured by an
    // older rule are stale and measured again.
    std::snprintf(buf, sizeof(buf), "r3,%lld,%lld,%lld,%016llx", us(data.silence.threshold_db), us(data.silence.preroll_ms),
                  us(data.silence.tail_ms), static_cast<unsigned long long>(h));
    std::string key = buf;
    // 13-62: the parts' own pre-roll / tail ("-" = the Sound's); nothing added when no part owns one.
    for (const PartSettings& p : data.parts) {
        if (!p.preroll_ms && !p.tail_ms) continue;
        key += "," + p.value + ":" + (p.preroll_ms ? std::to_string(us(*p.preroll_ms)) : std::string("-")) + ":" +
               (p.tail_ms ? std::to_string(us(*p.tail_ms)) : std::string("-"));
    }
    std::snprintf(buf, sizeof(buf), "|%lld,%llu", file.time, file.size);
    return key + buf;
}

bool PartTrimsStale(const SoundData& data, int variation, const FileVersion* file)
{
    if (!ValidIndex(data, variation) || !PartMarginsApply(data)) return false;
    const std::vector<PartSpan> secs = SectionBounds(data, variation);
    if (secs.empty()) return false;
    if (!TrimsFit(data, variation, secs)) return true;
    const std::string& have = data.frames[static_cast<size_t>(variation)].part_trim_key;
    const std::string  want = PartTrimKey(data, variation, file ? *file : FileVersion{});
    const size_t       hb = have.find('|'), wb = want.find('|');
    if (hb == std::string::npos || have.compare(0, hb, want, 0, wb) != 0) return true;  // settings or sections
    if (!file || *file == FileVersion{}) return false;
    return have.compare(hb, std::string::npos, want, wb, std::string::npos) != 0;      // another file version
}

std::vector<PartSection> PartSectionsOf(const SoundData& data, int variation)
{
    std::vector<PartSection>    out;
    const std::vector<PartSpan> secs = SectionBounds(data, variation);
    if (secs.empty()) return out;
    const SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    // The automatic edges: measured (the trims, while they fit and are not stale) when the margins apply, else
    // the whole section (not measured yet, or a mode without margins).
    const bool measured = PartMarginsApply(data) && !PartTrimsStale(data, variation);
    for (size_t i = 0; i < secs.size(); ++i) {
        PartSection s;
        s.value_uid = secs[i].value_uid;
        s.start = secs[i].start;
        s.end = secs[i].start + secs[i].length;
        s.measured = measured;
        s.placed_start = measured ? f.part_trims[2 * i] : s.start;
        s.placed_end = measured ? f.part_trims[2 * i + 1] : s.end;
        const bool   dropped = s.placed_end - s.placed_start <= kTimeEps;
        const double hs = HandEdgeIn(f, -1, s.start, s.end, s.value_uid), he = HandEdgeIn(f, 1, s.start, s.end, s.value_uid);
        if (dropped && (std::isfinite(hs) || std::isfinite(he))) {  // a hand edge brings a silent section back
            s.placed_start = s.start;
            s.placed_end = s.end;
        }
        if (std::isfinite(hs)) {
            s.placed_start = std::min(std::max(hs, s.start), s.end);
            s.hand_start = true;
        }
        if (std::isfinite(he)) {
            s.placed_end = std::min(std::max(he, s.start), s.end);
            s.hand_end = true;
        }
        if (s.placed_end < s.placed_start) s.placed_end = s.placed_start;  // the edges crossed: nothing placed
        out.push_back(s);
    }
    return out;
}

std::vector<PartSpan> PartPieces(const SoundData& data, int variation)
{
    if (!IsDivided(data) || data.mode == DetectMode::Takes) return PartSpansOf(data, variation);
    // 13-62g: each section's piece (its margins or hand edges), minus the excluded stretches inside it.
    std::vector<PartSpan>        out;
    const std::vector<EditPiece> edit = EditPiecesOf(data, variation);
    for (const PartSection& s : PartSectionsOf(data, variation)) {
        if (s.placed_end - s.placed_start <= kTimeEps) continue;
        for (const EditPiece& ep : edit) {
            if (ep.excluded) continue;
            const double a = std::max(s.placed_start, ep.start), e = std::min(s.placed_end, ep.start + ep.length);
            if (e - a <= kTimeEps) continue;
            if (!out.empty() && out.back().value_uid == s.value_uid && std::fabs(out.back().start + out.back().length - a) <= kTimeEps)
                out.back().length = e - out.back().start;
            else
                out.push_back({s.value_uid, a, e - a});
        }
    }
    return out;
}

std::vector<PartSpan> PlacedSpans(const SoundData& data, int variation, const std::string& part_value)
{
    std::vector<PartSpan> out;
    if (!part_value.empty()) {
        for (const PartSpan& s : PartPieces(data, variation))
            if (s.value_uid == part_value) out.push_back(s);
        return out;
    }
    for (const PartSpan& s : PartSpansOf(data, variation)) {
        if (!out.empty() && std::fabs(out.back().start + out.back().length - s.start) <= kTimeEps)
            out.back().length = s.start + s.length - out.back().start;
        else
            out.push_back({std::string(), s.start, s.length});
    }
    return out;
}


// ---- 13-62g: sections, gaps, piece edges moved by hand, the parts' invariant ---------------------------------

namespace {

// The hand edges of one side held by section [s, e] removed; how many went.
size_t EraseHandEdges(SoundFrame& f, int side, double s, double e)
{
    return DropHandEdges(f, side, [&](double h, const std::string&) { return !std::isfinite(h) || EdgeHeld(side, h, s, e); });
}

void AddHandEdge(SoundFrame& f, int side, double t, const std::string& part)
{
    EdgePartsOf(f, side).push_back(part);
    EdgesOf(f, side).push_back(t);
}

std::string SectionOwnerAt(const SoundData& data, const SoundFrame& f, double t, int side)
{
    const std::vector<PartSpan> secs = SectionBoundsOf(data, f);
    const int                   i = SectionHolding(secs, side, t);
    return i < 0 ? std::string() : secs[static_cast<size_t>(i)].value_uid;
}

void PinOuterEdges(SoundData& data, int variation, bool start, bool end)
{
    if (!ValidIndex(data, variation)) return;
    SoundFrame&  f = data.frames[static_cast<size_t>(variation)];
    const double fs = f.start, fe = FrameEnd(f);
    for (int side : {-1, 1})
        DropHandEdges(f, side, [&](double h, const std::string&) { return !(h >= fs - kTimeEps && h <= fe + kTimeEps); });
    const std::vector<PartSpan> secs = SectionBounds(data, variation);
    if (secs.empty()) return;
    if (start) {
        EraseHandEdges(f, -1, secs.front().start, secs.front().start + secs.front().length);
        AddHandEdge(f, -1, fs, secs.front().value_uid);
    }
    if (end) {
        EraseHandEdges(f, 1, secs.back().start, secs.back().start + secs.back().length);
        AddHandEdge(f, 1, fe, secs.back().value_uid);
    }
}

// 13-62g review (a hand edge belongs to its section): after a gesture that changed the variation's sections
// (`before` = the Sound before it, same variation index and edges), a hand edge stays only while the section holding
// it is of the same part and its wall on the edge's side did not move outward (a cut moved away or a stretch joined
// it there: what comes in there is placed by the automatic edge, never hidden by the old hand one). A frame edge
// stays a frame edge.
void AnchorHandEdges(const SoundData& before, SoundData& data, int variation)
{
    if (!ValidIndex(before, variation) || !ValidIndex(data, variation)) return;
    const SoundFrame&           bf = before.frames[static_cast<size_t>(variation)];
    SoundFrame&                 f = data.frames[static_cast<size_t>(variation)];
    const std::vector<PartSpan> sb = SectionBoundsOf(before, bf), sa = SectionBoundsOf(data, f);
    for (int side : {-1, 1})
        DropHandEdges(f, side, [&](double h, const std::string& part) {
            const int ib = SectionHolding(sb, side, h), ia = SectionHolding(sa, side, h);
            if (ib < 0 || ia < 0) return true;
            const PartSpan &b = sb[static_cast<size_t>(ib)], &a = sa[static_cast<size_t>(ia)];
            if (a.value_uid != b.value_uid || (!part.empty() && part != a.value_uid)) return true;
            if (side < 0) {
                const bool edge = std::fabs(b.start - bf.start) <= kTimeEps && std::fabs(a.start - f.start) <= kTimeEps;
                return !edge && a.start < b.start - kTimeEps;
            }
            const bool edge = std::fabs(b.start + b.length - FrameEnd(bf)) <= kTimeEps &&
                              std::fabs(a.start + a.length - FrameEnd(f)) <= kTimeEps;
            return !edge && a.start + a.length > b.start + b.length + kTimeEps;
        });
}

// 13-62g review: whether hand edge `h` (side) now hides sound it did not before: its section now (`sa`) is not of the
// part its section before (`sb`) was, or the stretch it hides beyond its old section's wall has a bin at or above
// the threshold (a cut moved away over a neighbour's sound, a variation grown over the next one). Held by no section
// now: left to TidyHandEdges.
bool HidesNewSound(const SoundData& data, const std::vector<PartSpan>& sa, const std::vector<PartSpan>& sb, int side, double h,
                   const PeakData& peaks)
{
    const int ia = SectionHolding(sa, side, h), ib = SectionHolding(sb, side, h);
    if (ia < 0) return false;
    if (ib < 0) return true;
    const PartSpan &a = sa[static_cast<size_t>(ia)], &b = sb[static_cast<size_t>(ib)];
    if (a.value_uid != b.value_uid) return true;
    const double lo = side < 0 ? a.start : std::max(h, b.start + b.length);
    const double hi = side < 0 ? std::min(h, b.start) : a.start + a.length;
    if (hi - lo <= kTimeEps) return false;
    long long k0 = 0, k1 = 0;
    BinRange(peaks, lo, hi, &k0, &k1);
    for (long long k = k0; k < k1; ++k)
        if (BinDb(peaks, k) >= data.silence.threshold_db) return true;
    return false;
}

// 13-62g review: each hand edge in a section of its own part: one whose part is not known yet takes the part of the
// section holding it; one held by no section or by another part's goes; a second one in a section goes (the first
// one held acts).
void TidyHandEdges(SoundData& data, int variation)
{
    SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    if (!f.on) return;  // an off variation keeps them for when it is on again
    const std::vector<PartSpan> secs = SectionBounds(data, variation);
    for (int side : {-1, 1}) {
        std::vector<std::string>& parts = EdgePartsOf(f, side);
        const std::vector<double>& edges = EdgesOf(f, side);
        for (size_t j = 0; j < edges.size(); ++j) {
            const int i = SectionHolding(secs, side, edges[j]);
            if (i >= 0 && parts[j].empty()) parts[j] = secs[static_cast<size_t>(i)].value_uid;
        }
        std::set<int> seen;
        DropHandEdges(f, side, [&](double h, const std::string& part) {
            const int i = SectionHolding(secs, side, h);
            if (i < 0 || part != secs[static_cast<size_t>(i)].value_uid) return true;
            return !seen.insert(i).second;
        });
    }
}

// 13-62g review (Antho: a range given to a part is placed, as a REAPER item covering it): the section holding
// [a, b] after it was given to its part covers it. Where that part placed nothing of that section before on a side
// of the range (`before`'s pieces now joined into it), the edge on that side is pinned by hand on the range's edge
// (a hand edge already further out stays); where it did, its automatic edge there already reaches past the range
// (the same sound and margin).
void CoverGivenRange(const SoundData& before, SoundData& data, int variation, double a, double b)
{
    const std::vector<PartSection> secs = PartSectionsOf(data, variation);
    const double                   mid = (a + b) * 0.5;
    const PartSection*             s = nullptr;
    for (const PartSection& x : secs)
        if (mid >= x.start && mid < x.end) s = &x;
    if (!s) return;
    a = std::max(a, s->start);
    b = std::min(b, s->end);
    if (a - s->start <= kMinPiece) a = s->start;  // a snapped edge: from the wall
    if (s->end - b <= kMinPiece) b = s->end;
    bool placed_before = false, placed_after = false;
    if (ValidIndex(before, variation))
        for (const PartSection& o : PartSectionsOf(before, variation)) {
            if (o.value_uid != s->value_uid || o.placed_end - o.placed_start <= kTimeEps) continue;
            if (o.placed_start < s->start - kTimeEps || o.placed_end > s->end + kTimeEps) continue;  // not joined here
            if (o.placed_start < a - kTimeEps) placed_before = true;
            if (o.placed_end > b + kTimeEps) placed_after = true;
        }
    SoundFrame&       f = data.frames[static_cast<size_t>(variation)];
    const std::string owner = s->value_uid;
    const double      ss = s->start, se = s->end;
    const bool        hs = s->hand_start, he = s->hand_end;
    const double      ps = s->placed_start, pe = s->placed_end;
    if (!placed_before && !(hs && ps <= a + kTimeEps)) {
        EraseHandEdges(f, -1, ss, se);
        AddHandEdge(f, -1, a, owner);
    }
    if (!placed_after && !(he && pe >= b - kTimeEps)) {
        EraseHandEdges(f, 1, ss, se);
        AddHandEdge(f, 1, b, owner);
    }
}

// The section of the variation holding `t` (index into PartSectionsOf), -1 when none.
int SectionIndexAt(const std::vector<PartSection>& secs, double t)
{
    for (size_t i = 0; i < secs.size(); ++i)
        if (t >= secs[i].start - kTimeEps && t < secs[i].end - kTimeEps) return static_cast<int>(i);
    if (!secs.empty() && std::fabs(t - secs.back().end) <= kTimeEps) return static_cast<int>(secs.size()) - 1;
    return -1;
}

}  // namespace

SilenceParams DetectionSilence(const SoundData& data)
{
    SilenceParams sp = data.silence;
    if (!PartMarginsApply(data)) return sp;
    for (const PartSettings& p : data.parts) {
        if (PartIndex(data, p.value) < 0) continue;
        if (p.preroll_ms && std::isfinite(*p.preroll_ms)) sp.preroll_ms = std::max(sp.preroll_ms, *p.preroll_ms);
        if (p.tail_ms && std::isfinite(*p.tail_ms)) sp.tail_ms = std::max(sp.tail_ms, *p.tail_ms);
    }
    return sp;
}

GapInfo GapAt(const SoundData& data, int variation, double t)
{
    GapInfo g;
    if (!ValidIndex(data, variation) || !std::isfinite(t)) return g;
    const SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    if (!f.on || t < f.start || t >= FrameEnd(f)) return g;
    std::string own;
    if (!PartAt(data, variation, t, &own)) {  // excluded: the run of excluded pieces around t
        g.kind = GapKind::Excluded;
        g.start = g.end = t;
        const std::vector<EditPiece> ps = EditPiecesOf(data, variation);
        for (size_t k = 0; k < ps.size(); ++k) {
            if (!(t >= ps[k].start && t < ps[k].start + ps[k].length)) continue;
            size_t lo = k, hi = k;
            while (lo > 0 && ps[lo - 1].excluded) --lo;
            while (hi + 1 < ps.size() && ps[hi + 1].excluded) ++hi;
            g.start = ps[lo].start;
            g.end = ps[hi].start + ps[hi].length;
        }
        return g;
    }
    const std::vector<PartSection> secs = PartSectionsOf(data, variation);
    const int                      i = SectionIndexAt(secs, t);
    if (i < 0) return g;
    const PartSection& s = secs[static_cast<size_t>(i)];
    if (t >= s.placed_start && t < s.placed_end) return g;  // placed
    g.value = s.value_uid;
    if (s.placed_end - s.placed_start <= kTimeEps && !s.hand_start && !s.hand_end) {  // review: a hand edge says why
        g.kind = GapKind::NoSound;
        g.start = s.start;
        g.end = s.end;
    } else if (t < s.placed_start) {
        g.kind = s.hand_start ? GapKind::HandEdge : GapKind::BeforeSound;
        g.start = s.start;
        g.end = s.placed_start;
        g.margin_ms = PartPrerollMs(data, s.value_uid);
    } else {
        g.kind = s.hand_end ? GapKind::HandEdge : GapKind::AfterSound;
        g.start = s.placed_end;
        g.end = s.end;
        g.margin_ms = PartTailMs(data, s.value_uid);
    }
    return g;
}

std::string GapTip(const GapInfo& gap, const std::string& part_name)
{
    const std::string& p = part_name;
    char               ms[32];
    std::snprintf(ms, sizeof(ms), "%.0f ms", gap.margin_ms);
    switch (gap.kind) {
        case GapKind::Excluded:
            return "Gap: excluded, never placed. Click it on a lane: that part plays it (its piece's edges set by hand over "
                   "it); or right-click \xE2\x80\xBA Include again.";
        case GapKind::AfterSound:
            return "Gap: after " + p + "'s sound and its Tail (" + ms + "), not placed. Raise " + p +
                   "'s Tail or drag its edge to fill it; or click it on another lane: that part plays it (its piece's "
                   "edges set by hand over it).";
        case GapKind::BeforeSound:
            return "Gap: before " + p + "'s sound and its Pre-roll (" + ms + "), not placed. Raise " + p +
                   "'s Pre-roll or drag its edge to fill it; or click it on another lane: that part plays it (its "
                   "piece's edges set by hand over it).";
        case GapKind::NoSound:
            return "Gap: " + p + " has nothing above the threshold here (it sounds elsewhere in this variation), not placed. "
                   "Lower the Threshold, or click it on another lane: that part plays it (its piece's edges set by hand "
                   "over it).";
        case GapKind::HandEdge:
            return "Gap: " + p + "'s edge was moved by hand, not placed. Drag the edge back, or double-click it for the "
                   "automatic edge.";
        case GapKind::None: break;
    }
    return std::string();
}

bool EditRangeAt(const SoundData& data, int variation, double t, EditPiece* out)
{
    for (const EditPiece& p : EditPiecesOf(data, variation)) {
        if (!(t >= p.start && t < p.start + p.length)) continue;
        EditPiece r = p;
        if (!p.excluded) {
            const GapInfo g = GapAt(data, variation, t);
            if (g.kind != GapKind::None && g.kind != GapKind::Excluded) {
                const double a = std::max(p.start, g.start), e = std::min(p.start + p.length, g.end);
                if (e - a > kTimeEps) {
                    r.start = a;
                    r.length = e - a;
                }
            }
        }
        if (out) *out = r;
        return true;
    }
    return false;
}

bool PieceEdgeAt(const SoundData& data, int variation, const std::string& value, double t, double tol, PieceEdgeHit* out)
{
    if (!ValidIndex(data, variation) || value.empty() || !std::isfinite(t)) return false;
    const SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    const double      fs = f.start, fe = FrameEnd(f);
    double            best = std::isfinite(tol) ? std::max(tol, 0.0) : 0.0;
    bool              found = false;
    for (const PartSection& s : PartSectionsOf(data, variation)) {
        if (s.value_uid != value || s.placed_end - s.placed_start <= kTimeEps) continue;
        for (int side : {-1, 1}) {
            const double edge = side < 0 ? s.placed_start : s.placed_end;
            if (std::fabs(edge - fs) <= 1e-6 || std::fabs(edge - fe) <= 1e-6) continue;  // the variation's trim handle
            const double d = std::fabs(t - edge);
            if (d > best) continue;
            best = d;
            found = true;
            if (out) {
                out->variation = variation;
                out->side = side;
                out->edge = edge;
                out->at = (s.start + s.end) * 0.5;
                out->lo = side < 0 ? s.start : std::min(s.placed_start + kMinPiece, s.end);
                out->hi = side < 0 ? std::max(s.placed_end - kMinPiece, s.start) : s.end;
                out->hand = side < 0 ? s.hand_start : s.hand_end;
                out->value = value;
            }
        }
    }
    return found;
}

bool SetPieceEdge(SoundData& data, int variation, double at, int side, double to)
{
    if (!ValidIndex(data, variation) || side == 0 || !std::isfinite(to)) return false;
    const std::vector<PartSection> secs = PartSectionsOf(data, variation);
    const int                      i = SectionIndexAt(secs, at);
    if (i < 0) return false;
    const PartSection& s = secs[static_cast<size_t>(i)];
    // Inside its section (never across a cut), and at least 4 ms from its other edge.
    const double lo = side < 0 ? s.start : std::min(s.placed_start + kMinPiece, s.end);
    const double hi = side < 0 ? std::max(s.placed_end - kMinPiece, s.start) : s.end;
    to = std::min(std::max(to, lo), hi);
    const double cur = side < 0 ? s.placed_start : s.placed_end;
    const bool   hand = side < 0 ? s.hand_start : s.hand_end;
    if (hand && std::fabs(cur - to) <= 1e-9) return false;
    // 13-62g review: an automatic edge "moved" by less than 1 ms stays automatic (no undo point).
    if (!hand && std::fabs(cur - to) < 0.001) return false;
    SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    EraseHandEdges(f, side < 0 ? -1 : 1, s.start, s.end);
    AddHandEdge(f, side < 0 ? -1 : 1, to, s.value_uid);
    data.edited = true;
    return true;
}

bool ResetPieceEdge(SoundData& data, int variation, double at, int side)
{
    if (!ValidIndex(data, variation) || side == 0) return false;
    const std::vector<PartSection> secs = PartSectionsOf(data, variation);
    const int                      i = SectionIndexAt(secs, at);
    if (i < 0) return false;
    const PartSection& s = secs[static_cast<size_t>(i)];
    SoundFrame&        f = data.frames[static_cast<size_t>(variation)];
    return EraseHandEdges(f, side < 0 ? -1 : 1, s.start, s.end) > 0;
}

std::string PartsProblem(const SoundData& data, const PeakData* peaks)
{
    char msg[256];
    for (size_t v = 0; v < data.frames.size(); ++v) {
        const SoundFrame& f = data.frames[v];
        const int         vi = static_cast<int>(v);
        const double      fs = f.start, fe = FrameEnd(f);
        if (v > 0 && f.start < FrameEnd(data.frames[v - 1]) - 1e-9) {
            std::snprintf(msg, sizeof(msg), "variation %d overlaps the previous one", vi);
            return msg;
        }
        if (!IsDivided(data) || data.mode == DetectMode::Takes || !f.on) continue;
        for (size_t k = 0; k < f.cuts.size(); ++k)
            if (!(f.cuts[k] > fs && f.cuts[k] < fe) || (k > 0 && !(f.cuts[k] > f.cuts[k - 1]))) {
                std::snprintf(msg, sizeof(msg), "variation %d: cut %d out of order or outside", vi, static_cast<int>(k));
                return msg;
            }
        if (!f.owners.empty() && !OwnersSane(data, f)) {
            std::snprintf(msg, sizeof(msg), "variation %d: owners do not fit its cuts", vi);
            return msg;
        }
        // Every stretch is a piece that is either excluded or owned by a divide value; the pieces tile the frame.
        const std::vector<EditPiece> ep = EditPiecesOf(data, vi);
        double                       at = fs;
        for (const EditPiece& p : ep) {
            if (std::fabs(p.start - at) > 1e-9 || !(p.length > 0.0) || (!p.excluded && PartIndex(data, p.owner) < 0) ||
                (p.excluded && !p.owner.empty())) {
                std::snprintf(msg, sizeof(msg), "variation %d: piece at %.6f is nobody's or leaves a hole", vi, p.start);
                return msg;
            }
            at = p.start + p.length;
        }
        if (!ep.empty() && std::fabs(at - fe) > 1e-9) {
            std::snprintf(msg, sizeof(msg), "variation %d: its pieces stop before its end", vi);
            return msg;
        }
        // What is placed: in time order, never overlapping, inside the variation, each wholly owned by its part
        // (never over an excluded stretch or another part's section).
        double prev = -std::numeric_limits<double>::infinity();
        for (const PartSpan& s : PartPieces(data, vi)) {
            const double a = s.start, e = s.start + s.length;
            if (!(s.length > 0.0) || a < prev - 1e-9 || a < fs - 1e-9 || e > fe + 1e-9 || PartIndex(data, s.value_uid) < 0) {
                std::snprintf(msg, sizeof(msg), "variation %d: placed piece at %.6f overlaps or leaves its variation", vi, a);
                return msg;
            }
            double covered = 0.0;
            for (const EditPiece& p : ep)
                if (!p.excluded && p.owner == s.value_uid)
                    covered += std::max(0.0, std::min(e, p.start + p.length) - std::max(a, p.start));
            if (covered < s.length - 1e-7) {
                std::snprintf(msg, sizeof(msg), "variation %d: placed piece at %.6f covers what its part does not own", vi, a);
                return msg;
            }
            prev = e;
        }
        // 13-62g review: each hand edge sits in a section of its own part.
        const std::vector<PartSpan> secs = SectionBounds(data, vi);
        for (int side : {-1, 1}) {
            const std::vector<double>&      es = side < 0 ? f.edge_in : f.edge_out;
            const std::vector<std::string>& ps = side < 0 ? f.edge_in_part : f.edge_out_part;
            for (size_t j = 0; j < es.size(); ++j) {
                const int i = SectionHolding(secs, side, es[j]);
                if (i < 0 || (j < ps.size() && !ps[j].empty() && ps[j] != secs[static_cast<size_t>(i)].value_uid)) {
                    std::snprintf(msg, sizeof(msg), "variation %d: hand edge %.6f outside a section of its part", vi, es[j]);
                    return msg;
                }
            }
        }
        // 13-62g review, no sound lost: every bin at or above the threshold of an owned, not excluded stretch is
        // placed, unless a hand edge of its own section leaves it out on that side.
        if (!HasPeaks(peaks)) continue;
        for (const PartSection& s : PartSectionsOf(data, vi)) {
            long long lo = 0, hi = 0;
            BinRange(*peaks, s.start, s.end, &lo, &hi);
            for (long long k = lo; k < hi; ++k) {
                if (BinDb(*peaks, k) < data.silence.threshold_db) continue;
                const double t = std::max(static_cast<double>(k) / peaks->rate, s.start);
                if (!PartAt(data, vi, t, nullptr)) continue;  // excluded
                if (t >= s.placed_start - kTimeEps && t < s.placed_end - kTimeEps) continue;
                if ((t < s.placed_start && s.hand_start) || (t >= s.placed_end - kTimeEps && s.hand_end)) continue;
                std::snprintf(msg, sizeof(msg), "variation %d: sound at %.6f of a section %.6f..%.6f not placed", vi, t, s.start,
                              s.end);
                return msg;
            }
        }
    }
    return std::string();
}

std::vector<Onset> FindOnsets(const PeakData& peaks, double start, double length)
{
    std::vector<Onset> cand;
    if (!HasPeaks(&peaks) || !(length > 0.0)) return cand;
    long long lo = 0, hi = 0;
    BinRange(peaks, start, start + length, &lo, &hi);
    const long long win = std::max(1LL, static_cast<long long>(std::llround(kOnsetWindow * peaks.rate)));
    for (long long i = lo; i < hi; ++i) {
        const double v = BinDb(peaks, i);
        const double prev = i > 0 ? BinDb(peaks, i - 1) : kBeforeSourceDb;
        if (!(v > prev && v >= BinDb(peaks, i + 1))) continue;  // a local maximum (plateau: its first bin)
        double    mn = 0.0;
        long long at = -1;
        for (long long j = std::max(0LL, i - win); j < i; ++j) {
            const double d = BinDb(peaks, j);
            if (at < 0 || d <= mn) {
                mn = d;
                at = j;
            }
        }
        if (at < 0) {  // the source's first bin
            mn = kBeforeSourceDb;
            at = i - 1;
        }
        if (v - mn < kOnsetRiseDb) continue;
        Onset o;
        o.t = static_cast<double>(i) / peaks.rate;
        o.db = v;
        o.rise = static_cast<double>(at + 1) / peaks.rate;
        cand.push_back(o);
    }
    // >= 20 ms apart: the louder first (the earlier on a tie).
    std::vector<Onset> by_level = cand;
    std::stable_sort(by_level.begin(), by_level.end(), [](const Onset& a, const Onset& b) { return a.db > b.db; });
    std::vector<Onset> out;
    for (const Onset& o : by_level) {
        bool close = false;
        for (const Onset& k : out) close = close || std::fabs(k.t - o.t) < kOnsetSpacing - kTimeEps;
        if (!close) out.push_back(o);
    }
    std::sort(out.begin(), out.end(), [](const Onset& a, const Onset& b) { return a.t < b.t; });
    return out;
}

namespace {

// AutoCuts, with `rises` = each cut's onset rise in time order (empty for an even split).
std::vector<double> AutoCutsWithRises(const PeakData* peaks, const SoundFrame& frame, int parts, bool* unsure,
                                      std::vector<double>* rises)
{
    if (unsure) *unsure = false;
    if (rises) rises->clear();
    if (parts < 2) return {};
    if (HasPeaks(peaks)) {
        std::vector<Onset> ons = FindOnsets(*peaks, frame.start, frame.length);
        if (ons.size() >= static_cast<size_t>(parts)) {  // the first onset, then parts - 1 more
            std::vector<Onset> after(ons.begin() + 1, ons.end());
            std::stable_sort(after.begin(), after.end(), [](const Onset& a, const Onset& b) { return a.db > b.db; });
            const bool          q = parts == 2 && after.size() >= 2 && after[0].db - after[1].db < kUnsureDb;
            std::vector<double> cuts, r;
            for (int k = 0; k < parts - 1; ++k) {
                cuts.push_back(after[static_cast<size_t>(k)].rise - kCutLead);
                r.push_back(after[static_cast<size_t>(k)].rise);
            }
            if (SaneCuts(frame, cuts)) {
                if (unsure) *unsure = q;
                std::sort(r.begin(), r.end());
                if (rises) *rises = r;
                return cuts;
            }
        }
    }
    std::vector<double> even = EvenCuts(frame, parts);
    SaneCuts(frame, even);
    return even;
}

// 13-62g review (a part's Pre-roll must act): RAV's cuts of a divided Silence Sound sit max(5 ms, the later part's
// Pre-roll) before its onset's rise, never before the earlier part's sound end plus its Tail (when there is room
// for it; else they share the silence: never before its midpoint), never later than 5 ms before the rise.
void LeadCutsByPreroll(const SoundData& data, const PeakData& peaks, const SoundFrame& f, const std::vector<double>& rises,
                       std::vector<double>& cuts)
{
    if (!PartMarginsApply(data) || rises.size() != cuts.size()) return;
    const size_t n = data.divide_values.size();
    double       wall = f.start;
    for (size_t k = 0; k < cuts.size(); ++k) {
        const double      rise = rises[k], hi = cuts[k];
        const std::string later = data.divide_values[std::min(k + 1, n - 1)], earlier = data.divide_values[std::min(k, n - 1)];
        const double      lead = std::max(kCutLead, MarginS(PartPrerollMs(data, later)));
        const double      want = rise - lead;
        if (want < hi - kTimeEps) {
            long long lo_bin = 0, hi_bin = 0, last = -1;
            BinRange(peaks, wall, rise, &lo_bin, &hi_bin);
            for (long long b = lo_bin; b < hi_bin; ++b)
                if (BinDb(peaks, b) >= data.silence.threshold_db) last = b;
            const double sound_end = last >= 0 ? std::min(static_cast<double>(last + 1) / peaks.rate, rise) : wall;
            double       lo = sound_end + MarginS(PartTailMs(data, earlier));
            if (lo > hi) lo = std::min(hi, (sound_end + rise) * 0.5);
            cuts[k] = std::min(hi, std::max(want, lo));
        }
        wall = cuts[k];
    }
    SaneCuts(f, cuts);
}

}  // namespace

std::vector<double> AutoCuts(const PeakData* peaks, const SoundFrame& frame, int parts, bool* unsure)
{
    return AutoCutsWithRises(peaks, frame, parts, unsure, nullptr);
}

double ClampIntoSpans(const std::vector<PartSpan>& spans, double t)
{
    if (spans.empty() || InSpans(spans, t)) return t;
    double best = spans.front().start, best_d = std::numeric_limits<double>::infinity();
    for (const PartSpan& s : spans)
        for (double edge : {s.start, s.start + s.length}) {
            const double d = std::fabs(edge - t);
            if (d < best_d) {
                best_d = d;
                best = edge;
            }
        }
    return best;
}

double AutoSync(const SoundData& data, const PeakData* peaks, const std::vector<PartSpan>& spans)
{
    if (spans.empty()) return -1.0;
    double         base = spans.front().start;
    const SyncMode mode = PartSyncMode(data, spans.front().value_uid);  // 13-62: the part's own, else the Sound's
    if (mode != SyncMode::SliceStart && HasPeaks(peaks)) {
        double top = kSilentDb;
        for (const PartSpan& s : spans) {
            long long lo = 0, hi = 0;
            BinRange(*peaks, s.start, s.start + s.length, &lo, &hi);
            for (long long i = lo; i < hi; ++i) top = std::max(top, BinDb(*peaks, i));
        }
        if (top > kSilentDb) {
            // Attack: the first bin reaching the threshold, or 20 dB under the peak when that is higher
            // (never above the peak: a part quieter than the threshold still has an attack).
            const double want = mode == SyncMode::Peak
                                    ? top
                                    : std::min(top, std::max(data.silence.threshold_db, top - kAttackBelowPeakDb));
            bool found = false;
            for (const PartSpan& s : spans) {
                long long lo = 0, hi = 0;
                BinRange(*peaks, s.start, s.start + s.length, &lo, &hi);
                for (long long i = lo; i < hi && !found; ++i)
                    if (BinDb(*peaks, i) >= want - 1e-9) {
                        base = static_cast<double>(i) / peaks->rate;
                        found = true;
                    }
                if (found) break;
            }
        }
    }
    const double off_ms = PartSyncOffsetMs(data, spans.front().value_uid);
    const double off = std::isfinite(off_ms) ? off_ms / 1000.0 : 0.0;
    return ClampIntoSpans(spans, base + off);
}

namespace {

std::vector<PartSpan> SpansOfPart(const SoundData& data, int variation, int part)
{
    return PlacedSpans(data, variation, IsDivided(data) ? data.divide_values[static_cast<size_t>(part)] : std::string());
}

}  // namespace

namespace {

// 13-54: the variation's part trims measured on the peaks (none when the margins do not apply); without
// peaks the stored ones are kept while they still fit the spans, else dropped (measured at placement).
void RefreshTrims(SoundData& data, int variation, const PeakData* peaks)
{
    SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    if (!PartMarginsApply(data)) {
        f.part_trims.clear();
        f.part_trim_key.clear();
        return;
    }
    if (HasPeaks(peaks)) {
        f.part_trims.clear();
        for (const PartSpan& sp : MeasureSections(data, variation, *peaks)) {  // 13-62g: one pair per section
            f.part_trims.push_back(sp.start);
            f.part_trims.push_back(sp.start + sp.length);
        }
        f.part_trim_key = PartTrimKey(data, variation, peaks->file);  // 13-54 review
        return;
    }
    if (PartTrimsStale(data, variation)) {  // another threshold / margin / spans: measured at placement
        f.part_trims.clear();
        f.part_trim_key.clear();
    }
}

// 13-54 review: part `part`'s hand sync: the first hand sync its raw owner (PartAt: the cuts and zones,
// not the margins) gives to that part, clamped into `spans` (its pieces) 1 us inside an edge; -1 when
// none. A margin trimming its spot away keeps it in its own part, never hands it to a neighbour.
double HandSyncOf(const SoundData& data, int variation, int part, const std::vector<PartSpan>& spans)
{
    const SoundFrame&  f = data.frames[static_cast<size_t>(variation)];
    const std::string& value = IsDivided(data) ? data.divide_values[static_cast<size_t>(part)] : std::string();
    for (double h : f.hand_syncs) {
        std::string own;
        if (!PartAt(data, variation, h, &own) || own != value) continue;
        double s = ClampIntoSpans(spans, h);
        for (const PartSpan& sp : spans)
            if (InSpans({sp}, s)) {
                if (sp.length > 2.0 * kEdgeNudge) s = std::min(std::max(s, sp.start + kEdgeNudge), sp.start + sp.length - kEdgeNudge);
                break;
            }
        return s;
    }
    return -1.0;
}

}  // namespace

void RefreshFrame(SoundData& data, int variation, const PeakData* peaks)
{
    if (!ValidIndex(data, variation)) return;
    SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    if (data.mode == DetectMode::Takes) {  // A8: a take keeps its item's snap
        f.cuts.clear();
        f.cuts_hand = f.cut_unsure = false;
        f.syncs.clear();
        f.part_trims.clear();
        f.owners.clear();  // 13-62
        return;
    }
    const int  n = PartCount(data);
    const SoundFrame was = f;  // 13-62g review: its sections before RAV's cuts move (the hand edges follow them)
    // 13-62: a comped variation keeps its cuts, whatever their count; owners that do not fit go (the
    // legacy rule then applies).
    bool comped = n >= 2 && OwnersSane(data, f) && std::is_sorted(f.cuts.begin(), f.cuts.end());
    if (comped) {
        // 13-62 review: a trim that left a cut within 4 ms of the frame's edge (or past it) drops it with
        // the sliver piece it bounds, so no 1 ms piece is ever placed (SaneCuts would clamp it 1 ms in).
        const double fs = f.start, fe = FrameEnd(f);
        while (!f.cuts.empty() && f.cuts.front() <= fs + kMinPiece + kTimeEps) {
            f.cuts.erase(f.cuts.begin());
            f.owners.erase(f.owners.begin());  // the piece on its left
        }
        while (!f.cuts.empty() && f.cuts.back() >= fe - kMinPiece - kTimeEps) {
            f.cuts.pop_back();
            f.owners.pop_back();  // the piece on its right
        }
        comped = SaneCuts(f, f.cuts);
    }
    if (!comped) f.owners.clear();
    if (n < 2) {
        f.cuts.clear();
        f.cuts_hand = f.cut_unsure = false;
    } else if (comped) {
        f.cut_unsure = false;
    } else if (f.cuts_hand && f.cuts.size() == static_cast<size_t>(n - 1) && SaneCuts(f, f.cuts)) {
        f.cut_unsure = false;
    } else if (HasPeaks(peaks)) {
        bool                q = false;
        std::vector<double> rises;
        f.cuts = AutoCutsWithRises(peaks, f, n, &q, &rises);
        LeadCutsByPreroll(data, *peaks, f, rises, f.cuts);  // 13-62g review: the later part's Pre-roll
        f.cuts_hand = false;
        f.cut_unsure = q;
    } else if (!(f.cuts.size() == static_cast<size_t>(n - 1) && SaneCuts(f, f.cuts))) {
        f.cuts = EvenCuts(f, n);
        SaneCuts(f, f.cuts);
        f.cuts_hand = f.cut_unsure = false;
    }
    {   // 13-62g (the parts' invariant, PartsProblem): every cut strictly inside the frame, in order. A frame too
        // short for its parts' cuts keeps the ones that fit; owners that no longer fit go (the legacy rule).
        const double        fs = f.start, fe = FrameEnd(f);
        std::vector<double> kept;
        for (double c : f.cuts)
            if (std::isfinite(c) && c > fs + kTimeEps && c < fe - kTimeEps && (kept.empty() || c > kept.back() + kTimeEps))
                kept.push_back(c);
        if (kept.size() != f.cuts.size()) {
            f.cuts = kept;
            if (!f.owners.empty() && f.owners.size() != f.cuts.size() + 1) f.owners.clear();
        }
    }
    if (HasPeaks(peaks) && (f.cuts != was.cuts || f.owners != was.owners) && (!f.edge_in.empty() || !f.edge_out.empty())) {
        // 13-62g review: a cut that moved (a margin or threshold change, a trim) never makes a hand edge hide sound.
        const std::vector<PartSpan> sa = SectionBoundsOf(data, f), sb = SectionBoundsOf(data, was);
        for (int side : {-1, 1})
            DropHandEdges(f, side, [&](double h, const std::string&) { return HidesNewSound(data, sa, sb, side, h, *peaks); });
    }
    TidyHandEdges(data, variation);        // 13-62g review: each hand edge in a section of its own part
    RefreshTrims(data, variation, peaks);  // 13-54: before the syncs, measured over the trimmed parts
    const std::vector<double> old = f.syncs;
    std::vector<double>       syncs(static_cast<size_t>(n), -1.0);
    for (int k = 0; k < n; ++k) {
        const std::vector<PartSpan> spans = SpansOfPart(data, variation, k);
        if (spans.empty()) continue;
        // 13-62: measurable with the part's own sync mode.
        const bool measurable = PartSyncMode(data, spans.front().value_uid) == SyncMode::SliceStart || HasPeaks(peaks);
        double     s = HandSyncOf(data, variation, k, spans);  // 13-54 review: by its raw owner
        if (s < 0.0 && measurable) s = AutoSync(data, peaks, spans);
        if (s < 0.0 && old.size() == static_cast<size_t>(n) && old[static_cast<size_t>(k)] >= 0.0 &&
            InSpans(spans, old[static_cast<size_t>(k)]))
            s = old[static_cast<size_t>(k)];
        if (s < 0.0) {  // cannot be measured here: at placement
            f.syncs.clear();
            return;
        }
        syncs[static_cast<size_t>(k)] = s;
    }
    f.syncs = syncs;
}

void RefreshParts(SoundData& data, const PeakData* peaks)
{
    for (size_t i = 0; i < data.frames.size(); ++i) RefreshFrame(data, static_cast<int>(i), peaks);
}

bool SyncsStale(const SoundData& data, int variation)
{
    if (!ValidIndex(data, variation) || data.mode == DetectMode::Takes) return false;
    const SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    const int         n = PartCount(data);
    if (f.syncs.size() != static_cast<size_t>(n)) return true;
    if (PartTrimsStale(data, variation)) return true;
    for (int k = 0; k < n; ++k) {
        const std::vector<PartSpan> spans = SpansOfPart(data, variation, k);
        const double                s = f.syncs[static_cast<size_t>(k)];
        if (spans.empty() ? s >= 0.0 : !InSpans(spans, s)) return true;
    }
    return false;
}

bool MeasureSyncs(SoundData& data, int variation, const PeakData* peaks)
{
    if (!ValidIndex(data, variation) || data.mode == DetectMode::Takes) return false;
    if (!HasPeaks(peaks)) {  // 13-62: every part (its own sync mode) must be measurable without peaks
        if (PartSyncMode(data, std::string()) != SyncMode::SliceStart) return false;
        for (const PartSettings& p : data.parts)
            if (PartIndex(data, p.value) >= 0 && PartSyncMode(data, p.value) != SyncMode::SliceStart) return false;
    }
    RefreshTrims(data, variation, peaks);  // 13-54
    SoundFrame&         f = data.frames[static_cast<size_t>(variation)];
    const int           n = PartCount(data);
    std::vector<double> syncs(static_cast<size_t>(n), -1.0);
    for (int k = 0; k < n; ++k) {
        const std::vector<PartSpan> spans = SpansOfPart(data, variation, k);
        if (spans.empty()) continue;
        const double s = HandSyncOf(data, variation, k, spans);  // 13-54 review: by its raw owner
        syncs[static_cast<size_t>(k)] = s >= 0.0 ? s : AutoSync(data, peaks, spans);
    }
    f.syncs = syncs;
    return true;
}

bool ApplyMeasuredSyncs(SoundStore& store, const std::string& lib_uid, const std::string& node_uid,
                        const SoundData& measured, const std::set<int>& variations)
{
    SoundLibrary* lib = nullptr;
    for (SoundLibrary& L : store.libraries)
        if (L.uid == lib_uid) lib = &L;
    SoundNode* n = lib ? FindNode(*lib, node_uid) : nullptr;
    if (!n || n->type != SoundNodeType::Sound) return false;
    bool changed = false;
    for (int v : variations) {
        if (v < 0 || static_cast<size_t>(v) >= n->sound.frames.size() || static_cast<size_t>(v) >= measured.frames.size())
            continue;
        SoundFrame&       f = n->sound.frames[static_cast<size_t>(v)];
        const SoundFrame& g = measured.frames[static_cast<size_t>(v)];
        if (f.start != g.start || f.length != g.length ||
            (f.syncs == g.syncs && f.part_trims == g.part_trims && f.part_trim_key == g.part_trim_key))
            continue;
        f.syncs = g.syncs;
        f.part_trims = g.part_trims;  // 13-54
        f.part_trim_key = g.part_trim_key;
        changed = true;
    }
    return changed;
}

double SyncFor(const SoundData& data, int variation, const std::string& part_value)
{
    if (!ValidIndex(data, variation)) return -1.0;
    const SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    if (f.syncs.size() != static_cast<size_t>(PartCount(data))) return -1.0;
    if (part_value.empty()) {
        for (double s : f.syncs)
            if (s >= 0.0) return s;
        return -1.0;
    }
    const int k = PartIndex(data, part_value);
    return k < 0 ? -1.0 : f.syncs[static_cast<size_t>(k)];
}

void CarryHandMarks(std::vector<SoundFrame>& now, const std::vector<SoundFrame>& old, bool with_cuts)
{
    auto holder = [&](double t) -> SoundFrame* {
        for (SoundFrame& f : now)
            if (t >= f.start - kTimeEps && t <= FrameEnd(f) + kTimeEps) return &f;
        return nullptr;
    };
    // 13-62g review: a hand start goes to the frame with start <= h < end, a hand end to the one with start < h <= end
    // (a split pin on the boundary between two frames stays with its own one).
    auto edge_holder = [&](double h, int side) -> SoundFrame* {
        for (SoundFrame& f : now)
            if (EdgeHeld(side, h, f.start, FrameEnd(f))) return &f;
        return nullptr;
    };
    for (SoundFrame& f : now) {
        f.hand_syncs.clear();
        f.edge_in.clear();  // 13-62g: carried below with the cuts (a Reset clears them)
        f.edge_out.clear();
        f.edge_in_part.clear();
        f.edge_out_part.clear();
        if (with_cuts && (f.cuts_hand || !f.owners.empty())) {
            f.cuts.clear();
            f.cuts_hand = f.cut_unsure = false;
            f.owners.clear();  // 13-62: the owners go with their cuts
        }
    }
    auto owned = [](const SoundFrame& o) { return !o.owners.empty() && o.owners.size() == o.cuts.size() + 1; };
    std::vector<std::vector<const SoundFrame*>> into(now.size());  // the old frames whose cuts land in each
    for (const SoundFrame& o : old) {
        for (double h : o.hand_syncs) {
            SoundFrame* f = holder(h);
            if (f && std::find(f->hand_syncs.begin(), f->hand_syncs.end(), h) == f->hand_syncs.end()) f->hand_syncs.push_back(h);
        }
        // 13-62g: a piece edge moved by hand goes to the new frame holding it (as the hand cuts: kept across a
        // threshold or margin change, gone on Reset to detected).
        // 13-62g review: only into the new frame that overlaps the old one most (an edge left outside it by a shorter
        // re-detected frame never lands in the next variation).
        if (with_cuts) {
            SoundFrame* best = nullptr;
            double      best_ov = 0.0;
            for (SoundFrame& f : now) {
                const double ov = std::min(FrameEnd(f), FrameEnd(o)) - std::max(f.start, o.start);
                if (ov > best_ov) {
                    best_ov = ov;
                    best = &f;
                }
            }
            for (int side : {-1, 1}) {
                const std::vector<double>&      es = side < 0 ? o.edge_in : o.edge_out;
                const std::vector<std::string>& ps = side < 0 ? o.edge_in_part : o.edge_out_part;
                for (size_t j = 0; j < es.size(); ++j)
                    if (SoundFrame* f = edge_holder(es[j], side))
                        if (f == best) AddHandEdge(*f, side, es[j], j < ps.size() ? ps[j] : std::string());
            }
        }
        // 13-62: a comped frame's owners travel with its cuts (no cut: by its middle).
        const bool own = owned(o);
        if (!with_cuts || (!o.cuts_hand && !own) || (o.cuts.empty() && !own)) continue;
        const std::vector<double> marks = o.cuts.empty() ? std::vector<double>{o.start + o.length * 0.5} : o.cuts;
        SoundFrame*               f = holder(marks.front());
        bool                      all = f != nullptr;
        for (double c : marks) all = all && holder(c) == f;
        if (all) into[static_cast<size_t>(f - now.data())].push_back(&o);
    }
    for (size_t i = 0; i < now.size(); ++i) {
        std::vector<const SoundFrame*>& from = into[i];
        if (from.empty()) continue;
        SoundFrame& f = now[i];
        f.cuts_hand = true;
        f.cut_unsure = false;
        bool all_owned = true;
        for (const SoundFrame* o : from) all_owned = all_owned && owned(*o);
        if (from.size() == 1 || !all_owned) {  // one frame (or hand cuts with no owners): the first one's
            f.cuts = from.front()->cuts;
            if (owned(*from.front())) f.owners = from.front()->owners;
            continue;
        }
        // 13-62 review: several comped frames merged into one: their pieces in time order, a cut at each old
        // frame's start inside the new one (the stretch between two old frames stays with the earlier one's
        // last piece). A boundary that cannot be a cut (on or before the last one, or at the frame's edge)
        // joins the two pieces around it under the earlier owner.
        std::stable_sort(from.begin(), from.end(), [](const SoundFrame* x, const SoundFrame* y) { return x->start < y->start; });
        f.cuts = from.front()->cuts;
        f.owners = from.front()->owners;
        for (size_t k = 1; k < from.size(); ++k) {
            const SoundFrame& o = *from[k];
            const double      at = o.start;
            const bool        cut = at > f.start + kTimeEps && at < FrameEnd(f) - kTimeEps &&
                             (f.cuts.empty() || at > f.cuts.back() + kTimeEps) &&
                             (o.cuts.empty() || at < o.cuts.front() - kTimeEps);
            if (cut) {
                f.cuts.push_back(at);
                f.owners.push_back(o.owners.front());
            }
            f.cuts.insert(f.cuts.end(), o.cuts.begin(), o.cuts.end());
            f.owners.insert(f.owners.end(), o.owners.begin() + 1, o.owners.end());
        }
    }
}

void KeepCarriedHandEdges(const SoundData& before, SoundData& data, const PeakData* peaks)
{
    if (!HasPeaks(peaks)) return;
    for (size_t v = 0; v < data.frames.size(); ++v) {
        SoundFrame&                 f = data.frames[v];
        const std::vector<PartSpan> sa = SectionBounds(data, static_cast<int>(v));
        for (int side : {-1, 1})
            DropHandEdges(f, side, [&](double h, const std::string&) {
                for (const SoundFrame& o : before.frames)  // its old frame's section
                    if (EdgeHeld(side, h, o.start, FrameEnd(o))) return HidesNewSound(data, sa, SectionBoundsOf(before, o), side, h, *peaks);
                return true;
            });
    }
}

bool SetDivide(SoundData& data, const std::string& group, const std::vector<std::string>& values)
{
    const std::vector<std::string> vals = group.empty() ? std::vector<std::string>() : values;
    if (data.divide_group == group && data.divide_values == vals) return false;
    data.divide_group = group;
    data.divide_values = vals;
    for (SoundFrame& f : data.frames) {
        f.cuts.clear();
        f.cuts_hand = f.cut_unsure = false;
        f.syncs.clear();
        f.part_trims.clear();  // 13-54: measured again with the new parts
        f.owners.clear();      // 13-62 (decision 8)
        DropInnerHandEdges(f);  // 13-62g review: other parts, other sections
        f.edge_in_part.assign(f.edge_in.size(), std::string());  // its trimmed edges: the new parts adopt them
        f.edge_out_part.assign(f.edge_out.size(), std::string());
    }
    data.parts.clear();  // 13-62 (decision 8): the parts' own settings go with the old division
    auto known = [&](const std::string& v) { return std::find(vals.begin(), vals.end(), v) != vals.end(); };
    data.zones.erase(std::remove_if(data.zones.begin(), data.zones.end(),
                                    [&](const SoundZone& z) { return !z.to.empty() && !known(z.to); }),
                     data.zones.end());
    data.role_map.erase(std::remove_if(data.role_map.begin(), data.role_map.end(),
                                       [&](const RoleValue& r) { return !known(r.value); }),
                        data.role_map.end());
    return true;
}

bool SetSyncSettings(SoundData& data, SyncMode mode, double offset_ms)
{
    if (!std::isfinite(offset_ms) || (data.sync_mode == mode && data.sync_offset_ms == offset_ms)) return false;
    data.sync_mode = mode;
    data.sync_offset_ms = offset_ms;
    for (SoundFrame& f : data.frames) f.syncs.clear();
    return true;
}

bool AddZone(SoundData& data, double start, double length, const std::string& to)
{
    if (!std::isfinite(start) || !std::isfinite(length) || length <= kMinLen) return false;
    // 13-62g review: a "-> value" zone added or covered changes sections: their hand edges follow them.
    bool value_zones = !to.empty();
    for (const SoundZone& z : data.zones) value_zones = value_zones || !z.to.empty();
    const SoundData before = value_zones ? data : SoundData();
    const double end = start + length;
    data.zones.erase(std::remove_if(data.zones.begin(), data.zones.end(),
                                    [&](const SoundZone& z) {
                                        return z.start >= start - kTimeEps && z.start + z.length <= end + kTimeEps;
                                    }),
                     data.zones.end());
    SoundZone z;
    z.start = start;
    z.length = length;
    z.to = to;
    data.zones.push_back(z);
    if (value_zones)
        for (size_t i = 0; i < data.frames.size(); ++i) AnchorHandEdges(before, data, static_cast<int>(i));
    // 13-62g: the trims are measured per section, which an excluded zone never changes (PartTrimKey tells when a
    // "-> value" zone did): an exclusion only takes its own stretch away, even before the peaks are read.
    return true;
}

bool IncludeAgain(SoundData& data, double t)
{
    const size_t before = data.zones.size();
    data.zones.erase(std::remove_if(data.zones.begin(), data.zones.end(),
                                    [&](const SoundZone& z) {
                                        return z.to.empty() && t >= z.start - kTimeEps && t <= z.start + z.length + kTimeEps;
                                    }),
                     data.zones.end());
    if (data.zones.size() == before) return false;
    return true;  // 13-62g: the per-section trims still hold (an excluded zone never changes them)
}

bool MoveCut(SoundData& data, int variation, int k, double t)
{
    if (!ValidIndex(data, variation) || !std::isfinite(t)) return false;
    if (k < 0 || static_cast<size_t>(k) >= data.frames[static_cast<size_t>(variation)].cuts.size()) return false;
    const SoundData before = data;  // 13-62g review: the hand edges follow their sections
    if (!MoveCutRaw(data, variation, k, t)) return false;
    AnchorHandEdges(before, data, variation);
    return true;
}

namespace {

bool MoveCutRaw(SoundData& data, int variation, int k, double t)
{
    SoundFrame&  f = data.frames[static_cast<size_t>(variation)];
    const size_t i = static_cast<size_t>(k);
    // 13-62 (C4): on a comped variation, a cut dragged so a piece next to it would be under 4 ms removes
    // that piece and the cut; the piece on its other side takes the room.
    if (OwnersSane(data, f)) {
        const double left = i > 0 ? f.cuts[i - 1] : f.start, right = i + 1 < f.cuts.size() ? f.cuts[i + 1] : FrameEnd(f);
        const double c = std::min(std::max(t, left), right);
        const bool   left_small = c - left < kMinPiece, right_small = right - c < kMinPiece;
        if (left_small || right_small) {
            const bool drop_left = left_small && (!right_small || c <= f.cuts[i]);
            f.cuts.erase(f.cuts.begin() + static_cast<std::ptrdiff_t>(i));
            f.owners.erase(f.owners.begin() + static_cast<std::ptrdiff_t>(drop_left ? i : i + 1));
            f.cuts_hand = true;
            f.cut_unsure = false;
            f.part_trims.clear();
            return true;
        }
    }
    const double lo = (i > 0 ? f.cuts[i - 1] : f.start) + kMinLen;
    const double hi = (i + 1 < f.cuts.size() ? f.cuts[i + 1] : FrameEnd(f)) - kMinLen;
    if (lo > hi) return false;
    const double c = std::min(std::max(t, lo), hi);
    if (std::fabs(c - f.cuts[i]) <= kTimeEps && f.cuts_hand) return false;
    f.cuts[i] = c;
    f.cuts_hand = true;
    f.cut_unsure = false;
    f.part_trims.clear();  // 13-54: measured again around the moved cut (RefreshOne)
    return true;
}

}  // namespace

bool MoveSync(SoundData& data, int variation, int part, double t)
{
    if (!ValidIndex(data, variation) || !std::isfinite(t) || part < 0 || part >= PartCount(data)) return false;
    const std::vector<PartSpan> pieces = SpansOfPart(data, variation, part);
    if (pieces.empty()) return false;
    SoundFrame&       f = data.frames[static_cast<size_t>(variation)];
    const std::string value = IsDivided(data) ? data.divide_values[static_cast<size_t>(part)] : std::string();
    // 13-54 review: kept where the part both plays (its pieces) and owns the spot by the cuts and zones
    // (its raw spans), so the refresh gives it back to this part (a pre-roll reaching back over a cut
    // plays a neighbour's spot).
    std::vector<PartSpan> spans;
    for (const PartSpan& r : PartSpansOf(data, variation)) {
        if (r.value_uid != value) continue;
        for (const PartSpan& p : pieces) {
            const double a = std::max(r.start, p.start), b = std::min(r.start + r.length, p.start + p.length);
            if (b - a > kTimeEps) spans.push_back({value, a, b - a});
        }
    }
    if (spans.empty()) spans = pieces;
    double s = ClampIntoSpans(spans, t);
    // On a span edge (a cut) the sync would sit in both neighbouring parts: keep it 1 us inside its own.
    for (const PartSpan& sp : spans)
        if (InSpans({sp}, s)) {
            s = std::min(std::max(s, sp.start + kEdgeNudge), sp.start + sp.length - kEdgeNudge);
            break;
        }
    // Only the hand syncs this part owns go (13-54 review: by their raw owner, as HandSyncOf reads them).
    f.hand_syncs.erase(std::remove_if(f.hand_syncs.begin(), f.hand_syncs.end(),
                                      [&](double h) {
                                          std::string own;
                                          return PartAt(data, variation, h, &own) && own == value;
                                      }),
                       f.hand_syncs.end());
    f.hand_syncs.push_back(s);
    if (f.syncs.size() == static_cast<size_t>(PartCount(data))) f.syncs[static_cast<size_t>(part)] = s;
    return true;
}

bool ResetSyncs(SoundData& data, int variation)
{
    if (!ValidIndex(data, variation)) return false;
    SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    if (f.hand_syncs.empty()) return false;
    f.hand_syncs.clear();
    return true;
}

bool ResetCuts(SoundData& data, int variation)
{
    if (!ValidIndex(data, variation)) return false;
    SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    if (!f.cuts_hand && f.owners.empty()) return false;
    f.cuts.clear();
    f.cuts_hand = f.cut_unsure = false;
    f.part_trims.clear();
    f.owners.clear();  // 13-62: back to the legacy rule
    DropInnerHandEdges(f);  // 13-62g review: its sections all change
    return true;
}

std::string PartOfRole(const SoundStore& store, const SoundData& data, const std::string& role)
{
    if (role.empty() || !IsDivided(data)) return std::string();
    std::string value;
    if (!data.role_map.empty()) {
        for (const RoleValue& r : data.role_map)
            if (r.role == role) {
                value = r.value;
                break;
            }
    } else if (const SwitchGroup* g = FindGroup(store, data.divide_group)) {
        for (const SwitchValue& v : g->values)
            if (std::find(v.roles.begin(), v.roles.end(), role) != v.roles.end()) {
                value = v.uid;
                break;
            }
    }
    return PartIndex(data, value) >= 0 ? value : std::string();
}


// ---- 13-62: per-part settings and comping ----------------------------------------------------------------

namespace {

PartSettings* FindPart(SoundData& data, const std::string& value)
{
    if (value.empty()) return nullptr;
    for (PartSettings& p : data.parts)
        if (p.value == value) return &p;
    return nullptr;
}

int OwnCount(const PartSettings& p)
{
    return (p.preroll_ms ? 1 : 0) + (p.tail_ms ? 1 : 0) + (p.sync_mode ? 1 : 0) + (p.sync_offset_ms ? 1 : 0) +
           (p.gain_db ? 1 : 0) + (p.norm_mode ? 1 : 0) + (p.norm_target ? 1 : 0) + (p.mono ? 1 : 0) + (p.fade_in ? 1 : 0) +
           (p.fade_out ? 1 : 0);
}

// What a setting change asks of the measured data: a sync change drops the measured syncs, a pre-roll /
// tail change the part trims (RefreshParts measures them again; without peaks, placement does).
void Remeasure(SoundData& data, bool syncs, bool trims)
{
    for (SoundFrame& f : data.frames) {
        if (syncs) f.syncs.clear();
        if (trims) f.part_trims.clear();
    }
}

// An entry left with nothing of its own (and no unknown field) goes.
void PruneParts(SoundData& data)
{
    data.parts.erase(std::remove_if(data.parts.begin(), data.parts.end(),
                                    [](const PartSettings& p) { return OwnCount(p) == 0 && p.kept.empty(); }),
                     data.parts.end());
}

bool Finite(const std::optional<double>& v)
{
    return v && std::isfinite(*v);
}

}  // namespace

const PartSettings* PartSettingsOf(const SoundData& data, const std::string& value)
{
    if (value.empty()) return nullptr;
    for (const PartSettings& p : data.parts)
        if (p.value == value) return &p;
    return nullptr;
}

double PartGainDb(const SoundData& data, const std::string& value)
{
    const PartSettings* p = PartSettingsOf(data, value);
    return p && p->gain_db ? *p->gain_db : 0.0;
}

double PartPrerollMs(const SoundData& data, const std::string& value)
{
    const PartSettings* p = PartSettingsOf(data, value);
    return p && p->preroll_ms ? *p->preroll_ms : data.silence.preroll_ms;
}

double PartTailMs(const SoundData& data, const std::string& value)
{
    const PartSettings* p = PartSettingsOf(data, value);
    return p && p->tail_ms ? *p->tail_ms : data.silence.tail_ms;
}

SyncMode PartSyncMode(const SoundData& data, const std::string& value)
{
    const PartSettings* p = PartSettingsOf(data, value);
    return p && p->sync_mode ? *p->sync_mode : data.sync_mode;
}

double PartSyncOffsetMs(const SoundData& data, const std::string& value)
{
    const PartSettings* p = PartSettingsOf(data, value);
    return p && p->sync_offset_ms ? *p->sync_offset_ms : data.sync_offset_ms;
}

MonoMode PartMono(const SoundData& data, const std::string& value)
{
    const PartSettings* p = PartSettingsOf(data, value);
    return p && p->mono ? *p->mono : data.mono;
}

bool PartFades(const SoundData& data, const std::string& value, double* in, double* out)
{
    const PartSettings* p = PartSettingsOf(data, value);
    const bool          own_in = p && p->fade_in, own_out = p && p->fade_out;
    if (in) *in = own_in ? *p->fade_in : data.fade_in;
    if (out) *out = own_out ? *p->fade_out : data.fade_out;
    return own_in || own_out;
}

void SetPartSetting(SoundData& data, const std::string& value, const PartSettings& patch)
{
    if (PartIndex(data, value) < 0) return;
    PartSettings* p = FindPart(data, value);
    if (!p) {
        data.parts.emplace_back();
        p = &data.parts.back();
        p->value = value;
    }
    bool syncs = false, trims = false;
    if (Finite(patch.preroll_ms)) {
        p->preroll_ms = std::max(*patch.preroll_ms, 0.0);
        trims = true;
    }
    if (Finite(patch.tail_ms)) {
        p->tail_ms = std::max(*patch.tail_ms, 0.0);
        trims = true;
    }
    if (patch.sync_mode) {
        p->sync_mode = patch.sync_mode;
        syncs = true;
    }
    if (Finite(patch.sync_offset_ms)) {
        p->sync_offset_ms = patch.sync_offset_ms;
        syncs = true;
    }
    if (Finite(patch.gain_db)) p->gain_db = patch.gain_db;
    if (patch.norm_mode && *patch.norm_mode >= 0 && *patch.norm_mode < kNormModeCount) p->norm_mode = patch.norm_mode;
    if (Finite(patch.norm_target)) p->norm_target = std::min(std::max(*patch.norm_target, kNormTargetMin), kNormTargetMax);
    if (patch.mono) p->mono = patch.mono;
    if (Finite(patch.fade_in)) p->fade_in = std::max(*patch.fade_in, 0.0);
    if (Finite(patch.fade_out)) p->fade_out = std::max(*patch.fade_out, 0.0);
    PruneParts(data);
    Remeasure(data, syncs, trims);
}

void ResetPartSetting(SoundData& data, const std::string& value, PartField field)
{
    PartSettings* p = FindPart(data, value);
    if (!p) return;
    bool syncs = false, trims = false;
    switch (field) {
    case PartField::Preroll: trims = p->preroll_ms.has_value(); p->preroll_ms.reset(); break;
    case PartField::Tail: trims = p->tail_ms.has_value(); p->tail_ms.reset(); break;
    case PartField::SyncMode: syncs = p->sync_mode.has_value(); p->sync_mode.reset(); break;
    case PartField::SyncOffset: syncs = p->sync_offset_ms.has_value(); p->sync_offset_ms.reset(); break;
    case PartField::Gain: p->gain_db.reset(); break;
    case PartField::NormMode: p->norm_mode.reset(); break;
    case PartField::NormTarget: p->norm_target.reset(); break;
    case PartField::Mono: p->mono.reset(); break;
    case PartField::FadeIn: p->fade_in.reset(); break;
    case PartField::FadeOut: p->fade_out.reset(); break;
    }
    PruneParts(data);
    Remeasure(data, syncs, trims);
}

void ResetPartAll(SoundData& data, const std::string& value)
{
    PartSettings* p = FindPart(data, value);
    if (!p) return;
    const bool syncs = p->sync_mode || p->sync_offset_ms, trims = p->preroll_ms || p->tail_ms;
    data.parts.erase(data.parts.begin() + (p - data.parts.data()));
    Remeasure(data, syncs, trims);
}

int OwnSettingCount(const SoundData& data, const std::string& value)
{
    const PartSettings* p = PartSettingsOf(data, value);
    return p ? OwnCount(*p) : 0;
}

std::vector<EditPiece> EditPiecesOf(const SoundData& data, int variation)
{
    std::vector<EditPiece> out;
    if (!ValidIndex(data, variation)) return out;
    const SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    if (!f.on || !(f.length > 0.0)) return out;
    const std::vector<double> b = PieceBounds(data, f);
    for (size_t k = 0; k + 1 < b.size(); ++k) {
        const double a = b[k], e = b[k + 1];
        if (e - a <= kTimeEps) continue;
        EditPiece p;
        p.start = a;
        p.length = e - a;
        p.excluded = !PartAt(data, variation, (a + e) * 0.5, &p.owner);
        if (p.excluded) p.owner.clear();
        p.index = static_cast<int>(out.size());
        out.push_back(p);
    }
    return out;
}

bool AssignRange(SoundData& data, int variation, double a, double b, const std::string& value)
{
    if (!ValidIndex(data, variation) || data.mode == DetectMode::Takes || PartIndex(data, value) < 0 || !std::isfinite(a) ||
        !std::isfinite(b))
        return false;
    SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    if (!f.on) return false;
    if (a > b) std::swap(a, b);
    const double fs = f.start, fe = FrameEnd(f);
    // An edge within 4 ms of a cut or the frame's edge lands on it (no sliver piece).
    auto land = [&](double t) {
        t = std::min(std::max(t, fs), fe);
        double best = t, best_d = kMinPiece;
        std::vector<double> marks = f.cuts;
        marks.push_back(fs);
        marks.push_back(fe);
        for (const SoundZone& z : data.zones)  // 13-62 review: and a zone's edge inside the frame
            for (double e : {z.start, z.start + z.length})
                if (e > fs && e < fe) marks.push_back(e);
        for (double m : marks)
            if (std::fabs(m - t) < best_d) {
                best_d = std::fabs(m - t);
                best = m;
            }
        return best;
    };
    a = land(a);
    b = land(b);
    if (b - a < kMinPiece - kTimeEps) return false;
    const SoundData              before_data = data;  // 13-62g review: its sections and pieces before
    const SoundFrame             before = f;
    const std::vector<SoundZone> zones_before = data.zones;
    if (!OwnersSane(data, f)) f.owners = LegacyOwners(data, f);  // what PartAt shows now
    // Cuts at a and b (each piece split keeps its owner on both sides).
    for (double t : {a, b}) {
        if (t <= fs + kTimeEps || t >= fe - kTimeEps) continue;
        size_t k = 0;
        while (k < f.cuts.size() && f.cuts[k] < t - kTimeEps) ++k;
        if (k < f.cuts.size() && std::fabs(f.cuts[k] - t) <= kTimeEps) continue;
        const std::string o = f.owners[k];
        f.cuts.insert(f.cuts.begin() + static_cast<std::ptrdiff_t>(k), t);
        f.owners.insert(f.owners.begin() + static_cast<std::ptrdiff_t>(k), o);
    }
    for (size_t k = 0; k < f.owners.size(); ++k) {
        const double lo = k > 0 ? f.cuts[k - 1] : fs, hi = k < f.cuts.size() ? f.cuts[k] : fe;
        const double mid = (lo + hi) * 0.5;
        if (mid > a && mid < b) f.owners[k] = value;
    }
    // Zones lose what they cover inside [a, b] (an exclusion there is included again; a legacy "-> value"
    // zone no longer overrides the new owner). 13-62 review: a remnant under 4 ms goes too (no sliver).
    std::vector<SoundZone> zones;
    for (const SoundZone& z : data.zones) {
        const double zs = z.start, ze = z.start + z.length;
        if (ze <= a + kTimeEps || zs >= b - kTimeEps) {
            zones.push_back(z);
            continue;
        }
        if (a - zs >= kMinPiece - kTimeEps) {
            SoundZone l = z;
            l.length = a - zs;
            zones.push_back(l);
        }
        if (ze - b >= kMinPiece - kTimeEps) {
            SoundZone r = z;
            r.start = b;
            r.length = ze - b;
            zones.push_back(r);
        }
    }
    data.zones = zones;
    bool zones_same = zones.size() == zones_before.size();
    for (size_t i = 0; zones_same && i < zones.size(); ++i)
        zones_same = zones[i].start == zones_before[i].start && zones[i].length == zones_before[i].length &&
                     zones[i].to == zones_before[i].to;
    // No owner changes (a swipe over the lane's own part): left exactly as it was, no new cut, no undo point.
    bool same = zones_same;
    if (same) {
        SoundData probe = data;
        probe.frames[static_cast<size_t>(variation)] = before;
        probe.zones = zones_before;
        for (const EditPiece& p : EditPiecesOf(data, variation)) {
            std::string was;
            const bool  in = PartAt(probe, variation, p.start + p.length * 0.5, &was);
            same = same && in == !p.excluded && (!in || was == p.owner);
        }
    }
    if (same) {
        f = before;
        return false;
    }
    f.cuts_hand = true;
    f.cut_unsure = false;
    for (SoundFrame& x : data.frames) x.part_trims.clear();  // 13-54: the spans changed
    // 13-62g review: the hand edges follow their sections; the range given is placed (its section's edges pinned
    // over it, REAPER: the item covers it).
    AnchorHandEdges(before_data, data, variation);
    CoverGivenRange(before_data, data, variation, std::max(a, fs), std::min(b, fe));
    return true;
}

double SnapToCut(const SoundData& data, int variation, double t, double tolerance_s)
{
    if (!ValidIndex(data, variation) || !std::isfinite(t)) return t;
    const SoundFrame&   f = data.frames[static_cast<size_t>(variation)];
    std::vector<double> marks = {f.start, FrameEnd(f)};
    if (IsDivided(data)) marks.insert(marks.end(), f.cuts.begin(), f.cuts.end());
    for (const SoundZone& z : data.zones)  // 13-62 review: a zone's edge inside the frame snaps too
        for (double e : {z.start, z.start + z.length})
            if (e > f.start && e < FrameEnd(f)) marks.push_back(e);
    double best = t, best_d = std::isfinite(tolerance_s) ? std::max(tolerance_s, 0.0) : 0.0;
    for (double m : marks)
        if (std::fabs(m - t) <= best_d) {
            best_d = std::fabs(m - t);
            best = m;
        }
    return best;
}

bool ExcludeRange(SoundData& data, int variation, double a, double b)
{
    if (!ValidIndex(data, variation) || !std::isfinite(a) || !std::isfinite(b)) return false;
    const SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    if (!f.on) return false;  // 13-62 review: an off variation is excluded whole already
    if (a > b) std::swap(a, b);
    a = std::max(a, f.start);
    b = std::min(b, FrameEnd(f));
    if (!(b > a)) return false;
    // 13-62 review: nothing played left in [a, b] (already excluded): nothing to write, no undo point.
    bool played = false;
    for (const EditPiece& p : EditPiecesOf(data, variation))
        if (!p.excluded && std::min(b, p.start + p.length) - std::max(a, p.start) > 1e-4) played = true;
    return played && AddZone(data, a, b - a, std::string());
}

double SnapSpanEdge(const SoundData& data, double t, double tolerance_s)
{
    if (!std::isfinite(t)) return t;
    const double tol = std::isfinite(tolerance_s) ? std::max(tolerance_s, 0.0) : 0.0;
    double       best = t, best_d = tol;
    auto         consider = [&](double c) {
        const double d = std::fabs(c - t);
        if (c != t && d <= best_d) {
            best_d = d;
            best = c;
        }
    };
    for (size_t i = 0; i < data.frames.size(); ++i) consider(SnapToCut(data, static_cast<int>(i), t, tol));
    for (const SoundZone& z : data.zones) {
        consider(z.start);
        consider(z.start + z.length);
    }
    return best;
}

bool SameEditPieces(const SoundData& a, const SoundData& b)
{
    if (a.frames.size() != b.frames.size()) return false;
    for (size_t i = 0; i < a.frames.size(); ++i) {
        const std::vector<EditPiece> pa = EditPiecesOf(a, static_cast<int>(i)), pb = EditPiecesOf(b, static_cast<int>(i));
        if (pa.size() != pb.size()) return false;
        for (size_t k = 0; k < pa.size(); ++k)
            if (std::fabs(pa[k].start - pb[k].start) > 1e-9 || std::fabs(pa[k].length - pb[k].length) > 1e-9 ||
                pa[k].owner != pb[k].owner || pa[k].excluded != pb[k].excluded)
                return false;
    }
    return true;
}

CompClickPlan PlanCompClick(const SoundData& data, int variation, double t, const std::string& lane_value, bool ctrl,
                            bool clicked_empty)
{
    CompClickPlan plan;
    // 13-62g: in a stretch its part leaves out (after its tail...), the range is that gap only.
    if (variation >= 0) plan.on_piece = EditRangeAt(data, variation, t, &plan.piece);
    if (!lane_value.empty()) {  // a Split lane: its part, and a range it does not own taken whole
        plan.set_part = true;
        plan.part = lane_value;
        plan.take = plan.on_piece && !ctrl && (plan.piece.excluded || plan.piece.owner != lane_value);
    } else if (plan.on_piece && !plan.piece.excluded) {  // Combined / the comp strip: the part under the click
        plan.set_part = true;
        plan.part = plan.piece.owner;
    } else if (clicked_empty) {  // outside every variation: the Sound
        plan.set_part = true;
    }
    return plan;
}


// ---- part C: what the Edit window shows and its role gestures -----------------------------------------

int ExcludedZoneCount(const SoundData& data)
{
    int n = 0;
    for (const SoundZone& z : data.zones) n += z.to.empty() && z.length > 0.0 ? 1 : 0;
    return n;
}

int ExcludedStretchCount(const SoundData& data)
{
    int n = 0;
    for (size_t i = 0; i < data.frames.size(); ++i) {
        const SoundFrame& f = data.frames[i];
        if (!(f.length > 0.0)) continue;
        if (!f.on) {
            ++n;
            continue;
        }
        double at = f.start;  // the end of what the spans covered so far
        for (const PartSpan& s : PlacedSpans(data, static_cast<int>(i), std::string())) {
            if (s.start - at > kTimeEps) ++n;
            at = s.start + s.length;
        }
        if (FrameEnd(f) - at > kTimeEps) ++n;
    }
    return n;
}

int VariationsWithPart(const SoundData& data, const std::string& value_uid)
{
    int n = 0;
    for (size_t i = 0; i < data.frames.size(); ++i) n += PlacedSpans(data, static_cast<int>(i), value_uid).empty() ? 0 : 1;
    return n;
}

int HandSyncCount(const SoundData& data)
{
    int n = 0;
    for (const SoundFrame& f : data.frames) n += static_cast<int>(f.hand_syncs.size());
    return n;
}

int HandCutCount(const SoundData& data)
{
    int n = 0;
    if (!IsDivided(data)) return 0;
    for (const SoundFrame& f : data.frames) n += f.on && f.cuts_hand ? static_cast<int>(f.cuts.size()) : 0;
    return n;
}

int UnsureCutCount(const SoundData& data)
{
    int n = 0;
    if (!IsDivided(data)) return 0;
    for (const SoundFrame& f : data.frames) n += f.on && f.cut_unsure && !f.cuts_hand ? 1 : 0;
    return n;
}

bool SyncIsHand(const SoundData& data, int variation, int part)
{
    if (!ValidIndex(data, variation) || part < 0 || part >= PartCount(data)) return false;
    const SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    if (f.hand_syncs.empty()) return false;
    const std::vector<PartSpan> spans = SpansOfPart(data, variation, part);
    for (double h : f.hand_syncs)
        if (InSpans(spans, h)) return true;
    return false;
}

bool DivideStale(const SoundStore& store, const SoundData& data)
{
    if (!IsDivided(data)) return false;
    const SwitchGroup* g = FindGroup(store, data.divide_group);
    if (!g || g->values.size() != data.divide_values.size()) return true;
    for (size_t k = 0; k < g->values.size(); ++k)
        if (g->values[k].uid != data.divide_values[k]) return true;
    return false;
}

std::vector<std::vector<std::string>> RolesByValue(const SoundStore& store, const SoundData& data, bool* own)
{
    std::vector<std::vector<std::string>> out;
    if (own) *own = !data.role_map.empty();
    if (!IsDivided(data)) return out;
    out.resize(data.divide_values.size());
    if (!data.role_map.empty()) {
        for (const RoleValue& r : data.role_map) {
            const int k = PartIndex(data, r.value);
            if (k >= 0 && r.role != kNoRolesMark) out[static_cast<size_t>(k)].push_back(r.role);
        }
        return out;
    }
    if (const SwitchGroup* g = FindGroup(store, data.divide_group))
        for (const SwitchValue& v : g->values) {
            const int k = PartIndex(data, v.uid);
            if (k >= 0) out[static_cast<size_t>(k)] = v.roles;
        }
    return out;
}

bool ToggleSoundRole(const SoundStore& store, SoundData& data, const std::string& value_uid, const std::string& role)
{
    if (!IsDivided(data) || PartIndex(data, value_uid) < 0 || role.empty() || role == kNoRolesMark) return false;
    std::vector<RoleValue> m;
    if (data.role_map.empty()) {  // seeded from the group's, so the other roles keep their part
        const std::vector<std::vector<std::string>> by = RolesByValue(store, data);
        for (size_t k = 0; k < by.size(); ++k)
            for (const std::string& r : by[k]) m.push_back({r, data.divide_values[k]});
    } else {
        for (const RoleValue& r : data.role_map)
            if (r.role != kNoRolesMark) m.push_back(r);
    }
    bool had = false;
    for (const RoleValue& r : m) had = had || (r.role == role && r.value == value_uid);
    m.erase(std::remove_if(m.begin(), m.end(), [&](const RoleValue& r) { return r.role == role; }), m.end());
    if (!had) m.push_back({role, value_uid});
    if (m.empty()) m.push_back({kNoRolesMark, data.divide_values.front()});
    data.role_map = m;
    return true;
}

bool ToggleGroupRole(SoundStore& store, const std::string& group_uid, const std::string& value_uid, const std::string& role,
                     std::string* moved_from)
{
    if (moved_from) moved_from->clear();
    SwitchGroup* g = FindGroup(store, group_uid);
    if (!g || role.empty() || role == kNoRolesMark) return false;
    SwitchValue* target = nullptr;
    for (SwitchValue& v : g->values)
        if (v.uid == value_uid) target = &v;
    if (!target) return false;
    const bool had = std::find(target->roles.begin(), target->roles.end(), role) != target->roles.end();
    for (SwitchValue& v : g->values) {
        const auto it = std::find(v.roles.begin(), v.roles.end(), role);
        if (it == v.roles.end()) continue;
        if (&v != target && moved_from) *moved_from = v.uid;
        v.roles.erase(it);
    }
    if (!had) target->roles.push_back(role);
    return true;
}

// ---- 13-28: Normalize -------------------------------------------------------------------------------

namespace {

constexpr double kNormMaxAdjDb = 60.0;  // more than this = silence (spec 13-28 matrix)

bool UsableAdj(double db) { return std::isfinite(db) && db <= kNormMaxAdjDb; }

}  // namespace

double FrameGainDb(const SoundFrame& frame, const std::string& part_value)
{
    if (!part_value.empty()) {
        const auto it = frame.part_db.find(part_value);
        if (it != frame.part_db.end()) return it->second;
    }
    return frame.db;
}

double FrameLabelDb(const SoundData& data, int variation)
{
    if (!ValidIndex(data, variation)) return 0.0;
    const SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    if (NormPartsAvailable(data) && !f.part_db.empty()) {  // a take plays whole, at the frame's dB
        bool   any = false;
        double top = 0.0;
        for (const std::string& v : data.divide_values) {
            const auto it = f.part_db.find(v);
            if (it == f.part_db.end()) continue;
            top = any ? std::max(top, it->second) : it->second;
            any = true;
        }
        if (any) return top;
    }
    return f.db;
}

bool FrameClips(const SoundData& data, int variation, const PeakData& peaks)
{
    if (!ValidIndex(data, variation)) return false;
    const SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    if (!f.on) return false;
    std::vector<PartSpan> spans;
    if (data.mode == DetectMode::Takes) spans.push_back({std::string(), f.start, f.length});
    else spans = PartSpansOf(data, variation);
    for (const PartSpan& sp : spans) {
        const double pk = SpanPeakDb(peaks, sp.start, sp.length);
        // 13-62: plus the part's own gain (decision 3).
        if (std::isfinite(pk) && pk + FrameGainDb(f, sp.value_uid) + PartGainDb(data, sp.value_uid) > 1e-9) return true;
    }
    return false;
}

bool NormPartsAvailable(const SoundData& data)
{
    return IsDivided(data) && data.mode != DetectMode::Takes;
}

bool SameNormMeasured(const SoundData& x, const SoundData& y)
{
    const std::vector<SoundFrame>& a = x.frames;
    const std::vector<SoundFrame>& b = y.frames;
    if (a.size() != b.size() || x.zones.size() != y.zones.size() || x.role_map.size() != y.role_map.size() ||
        x.divide_group != y.divide_group || x.divide_values != y.divide_values || x.mode != y.mode)
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].start != b[i].start || a[i].length != b[i].length || a[i].on != b[i].on || a[i].cuts != b[i].cuts ||
            a[i].owners != b[i].owners)  // 13-62
            return false;
    for (size_t i = 0; i < x.zones.size(); ++i)
        if (x.zones[i].start != y.zones[i].start || x.zones[i].length != y.zones[i].length || x.zones[i].to != y.zones[i].to)
            return false;
    for (size_t i = 0; i < x.role_map.size(); ++i)
        if (x.role_map[i].role != y.role_map[i].role || x.role_map[i].value != y.role_map[i].value) return false;
    return true;
}

std::vector<NormRegion> NormalizeRegions(const SoundData& data, NormScope scope, const std::vector<int>& selection)
{
    std::vector<NormRegion> out;
    std::vector<int>        set;
    if (selection.empty()) {
        for (size_t i = 0; i < data.frames.size(); ++i) set.push_back(static_cast<int>(i));
    } else {
        for (int i : selection)
            if (ValidIndex(data, i) && std::find(set.begin(), set.end(), i) == set.end()) set.push_back(i);
        std::sort(set.begin(), set.end());
    }
    const bool takes = data.mode == DetectMode::Takes;
    const bool parts = scope == NormScope::Part && NormPartsAvailable(data);
    for (int i : set) {
        const SoundFrame& f = data.frames[static_cast<size_t>(i)];
        if (!f.on || !(f.length > 0.0)) continue;
        if (takes) {  // a take plays whole, in its own file
            NormRegion r;
            r.frame = i;
            r.spans.push_back({std::string(), f.start, f.length});
            out.push_back(r);
            continue;
        }
        if (!parts) {
            NormRegion r;
            r.frame = i;
            r.spans = PlacedSpans(data, i, std::string());
            if (!r.spans.empty()) out.push_back(r);
            continue;
        }
        const std::vector<PartSpan> spans = PartSpansOf(data, i);
        for (const std::string& v : data.divide_values) {
            NormRegion r;
            r.frame = i;
            r.part = v;
            for (const PartSpan& sp : spans)
                if (sp.value_uid == v) r.spans.push_back(sp);
            if (!r.spans.empty()) out.push_back(r);
        }
    }
    return out;
}

NormMode PartNormMode(const SoundData& data, const std::string& value, NormMode sound_mode)
{
    const PartSettings* p = value.empty() ? nullptr : PartSettingsOf(data, value);
    if (!p || !p->norm_mode || *p->norm_mode < 0 || *p->norm_mode >= kNormModeCount) return sound_mode;
    return static_cast<NormMode>(*p->norm_mode);
}

double PartNormTarget(const SoundData& data, const std::string& value, double sound_target)
{
    const PartSettings* p = value.empty() ? nullptr : PartSettingsOf(data, value);
    return p && p->norm_target && std::isfinite(*p->norm_target) ? *p->norm_target : sound_target;
}

std::vector<NormRegion> PartNormRegions(const SoundData& data, const std::string& value, const std::vector<int>& selection,
                                        NormMode sound_mode, double sound_target)
{
    std::vector<NormRegion> out;
    if (!NormPartsAvailable(data)) return out;
    for (NormRegion& r : NormalizeRegions(data, NormScope::Part, selection)) {
        if (r.part.empty() || (!value.empty() && r.part != value)) continue;
        r.mode = PartNormMode(data, r.part, sound_mode);
        r.target = PartNormTarget(data, r.part, sound_target);
        out.push_back(std::move(r));
    }
    return out;
}

void KeepOtherPartGains(const SoundData& before, std::vector<SoundFrame>& frames, const std::vector<NormRegion>& regions,
                        const std::vector<double>& adj_db)
{
    // The (frame, part) pairs found silent in this pass: they keep their gain but do not pull frame.db.
    std::set<std::pair<int, std::string>> silent;
    for (size_t k = 0; k < regions.size(); ++k)
        if (!regions[k].part.empty() && !(k < adj_db.size() && UsableAdj(adj_db[k])))
            silent.emplace(regions[k].frame, regions[k].part);
    for (size_t i = 0; i < frames.size() && i < before.frames.size(); ++i) {
        SoundFrame&       f = frames[i];
        const SoundFrame& b = before.frames[i];
        if (f.part_db == b.part_db && std::fabs(f.db - b.db) <= 1e-9) continue;  // not acted on
        for (const PartSpan& sp : PartSpansOf(before, static_cast<int>(i)))
            if (!sp.value_uid.empty() && !f.part_db.count(sp.value_uid)) f.part_db[sp.value_uid] = b.db;
        bool any = false;
        for (const auto& kv : f.part_db) {
            if (silent.count({static_cast<int>(i), kv.first})) continue;
            f.db = any ? std::min(f.db, kv.second) : kv.second;
            any = true;
        }
    }
}

double NormAdjustmentDb(double factor)
{
    if (!std::isfinite(factor) || factor <= 0.0) return std::numeric_limits<double>::quiet_NaN();
    const double db = 20.0 * std::log10(factor);
    return UsableAdj(db) ? db : std::numeric_limits<double>::quiet_NaN();
}

double CombineSpanAdjustments(NormMode mode, double target, const std::vector<double>& adj_db,
                              const std::vector<double>& durations)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const bool   integrated = mode == NormMode::RmsI || mode == NormMode::LufsI;
    double       best = nan, energy = 0.0, dur = 0.0;
    for (size_t k = 0; k < adj_db.size(); ++k) {
        const double a = adj_db[k];
        if (!UsableAdj(a)) continue;
        if (!integrated) {
            best = std::isfinite(best) ? std::min(best, a) : a;
            continue;
        }
        const double d = k < durations.size() && durations[k] > 0.0 ? durations[k] : 0.0;
        if (!(d > 0.0)) continue;
        energy += d * std::pow(10.0, (target - a) / 10.0);  // the span's level = target - its adjustment
        dur += d;
    }
    if (!integrated) return best;
    if (!(dur > 0.0) || !(energy > 0.0)) return nan;
    const double adj = target - 10.0 * std::log10(energy / dur);
    return UsableAdj(adj) ? adj : nan;
}

double RegionPeakAdjDb(const PeakData& peaks, const NormRegion& region, double target)
{
    double top = NegInf();
    for (const PartSpan& sp : region.spans) top = std::max(top, SpanPeakDb(peaks, sp.start, sp.length));
    if (!std::isfinite(top)) return std::numeric_limits<double>::quiet_NaN();
    const double adj = target - top;
    return UsableAdj(adj) ? adj : std::numeric_limits<double>::quiet_NaN();
}

NormalizeOutcome ApplyNormalize(std::vector<SoundFrame>& frames, NormScope scope, const std::vector<NormRegion>& regions,
                                const std::vector<double>& adj_db)
{
    NormalizeOutcome out;
    const std::vector<SoundFrame> before = frames;
    auto valid = [&](size_t i) {
        return i < adj_db.size() && UsableAdj(adj_db[i]) && regions[i].frame >= 0 &&
               static_cast<size_t>(regions[i].frame) < frames.size() && frames[static_cast<size_t>(regions[i].frame)].on;
    };
    if (scope == NormScope::Whole) {
        double low = 0.0;
        bool   any = false;
        for (size_t i = 0; i < regions.size(); ++i) {
            if (!valid(i)) continue;
            low = any ? std::min(low, adj_db[i]) : adj_db[i];
            any = true;
        }
        std::set<int> acted;
        for (size_t i = 0; i < regions.size(); ++i) {
            if (!valid(i)) {
                ++out.skipped;
                continue;
            }
            if (!acted.insert(regions[i].frame).second) continue;
            SoundFrame& f = frames[static_cast<size_t>(regions[i].frame)];
            f.db = low;
            f.part_db.clear();
            f.hand = true;
            ++out.done;
        }
    } else {
        for (size_t i = 0; i < regions.size(); ++i) {
            if (!valid(i)) {
                ++out.skipped;
                continue;
            }
            SoundFrame& f = frames[static_cast<size_t>(regions[i].frame)];
            if (scope == NormScope::Part && !regions[i].part.empty()) {
                f.part_db[regions[i].part] = adj_db[i];
            } else {
                f.db = adj_db[i];
                f.part_db.clear();
            }
            f.hand = true;
            ++out.done;
        }
        // Per part: the whole variation (a combined event, an unmapped role, a Blend layer "Whole", a
        // part not measured) plays at the frame's dB, so it follows its parts: the smallest of them,
        // the loudest part reaching the target. Left as it was, it kept an older gain (Antho 2026-10-13:
        // a box at +24.3 dB placed at +47).
        if (scope == NormScope::Part)
            for (size_t i = 0; i < regions.size(); ++i) {
                if (!valid(i) || regions[i].part.empty()) continue;
                SoundFrame& f = frames[static_cast<size_t>(regions[i].frame)];
                bool        any = false;
                for (const auto& kv : f.part_db) {
                    f.db = any ? std::min(f.db, kv.second) : kv.second;
                    any = true;
                }
            }
    }
    for (size_t i = 0; i < frames.size() && !out.changed; ++i) {
        const SoundFrame& a = frames[i];
        const SoundFrame& b = before[i];
        out.changed = std::fabs(a.db - b.db) > 1e-9 || a.hand != b.hand || a.part_db.size() != b.part_db.size();
        if (!out.changed)
            for (const auto& kv : a.part_db) {
                const auto it = b.part_db.find(kv.first);
                if (it == b.part_db.end() || std::fabs(it->second - kv.second) > 1e-9) out.changed = true;
            }
    }
    return out;
}

std::string DividedByText(const SoundStore& store, const SoundData& data)
{
    if (!IsDivided(data)) return {};
    const SwitchGroup* g = FindGroup(store, data.divide_group);
    std::string        vals;
    for (const std::string& v : data.divide_values) {
        std::string name = "?";
        if (g)
            for (const SwitchValue& x : g->values)
                if (x.uid == v) name = x.name;
        vals += (vals.empty() ? "" : " / ") + name;
    }
    return "Divided by " + (g ? g->name : std::string("?")) + " (" + vals + ")";
}

// ---- 13-62 (E2): the part rows' pure helpers ----

// Two entries own the same settings (nullptr = none): a gesture that changed nothing writes nothing.
bool SamePartSettings(const PartSettings* a, const PartSettings* b)
{
    if (!a || !b) return !a && !b;
    return a->preroll_ms == b->preroll_ms && a->tail_ms == b->tail_ms && a->sync_mode == b->sync_mode &&
           a->sync_offset_ms == b->sync_offset_ms && a->gain_db == b->gain_db && a->norm_mode == b->norm_mode &&
           a->norm_target == b->norm_target && a->mono == b->mono && a->fade_in == b->fade_in && a->fade_out == b->fade_out;
}

// A setting the parts' sync points or margins are measured with: its change measures them again.
bool PartFieldRemeasures(PartField f)
{
    return f == PartField::Preroll || f == PartField::Tail || f == PartField::SyncMode || f == PartField::SyncOffset;
}

// The part's own value of a number row, in the row's unit (fades: ms); none while it follows the Sound.
std::optional<double> OwnPartNumber(const PartSettings* p, PartField f)
{
    if (!p) return std::nullopt;
    switch (f) {
        case PartField::Preroll: return p->preroll_ms;
        case PartField::Tail: return p->tail_ms;
        case PartField::SyncOffset: return p->sync_offset_ms;
        case PartField::Gain: return p->gain_db;
        case PartField::NormTarget: return p->norm_target;
        case PartField::FadeIn: return p->fade_in ? std::optional<double>(*p->fade_in * 1000.0) : std::nullopt;
        case PartField::FadeOut: return p->fade_out ? std::optional<double>(*p->fade_out * 1000.0) : std::nullopt;
        default: return std::nullopt;
    }
}

PartSettings PartNumberPatch(PartField f, double v)
{
    PartSettings p;
    switch (f) {
        case PartField::Preroll: p.preroll_ms = v; break;
        case PartField::Tail: p.tail_ms = v; break;
        case PartField::SyncOffset: p.sync_offset_ms = v; break;
        case PartField::Gain: p.gain_db = v; break;
        case PartField::NormTarget: p.norm_target = v; break;
        case PartField::FadeIn: p.fade_in = v / 1000.0; break;
        case PartField::FadeOut: p.fade_out = v / 1000.0; break;
        default: break;
    }
    return p;
}

std::string RoundedNumberText(double v, bool sign)
{
    double r = std::round(v * 10.0) / 10.0;
    if (r == 0.0 || !std::isfinite(r)) r = 0.0;  // never "-0"
    char buf[48];
    std::snprintf(buf, sizeof(buf), sign && r > 0.0 ? "+%g" : "%g", r);
    return buf;
}

}  // namespace rav
