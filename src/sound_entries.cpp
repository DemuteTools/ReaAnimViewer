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
#include <set>
#include <sstream>

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

void TrimFrame(SoundData& data, int i, double start, double length, double source_len)
{
    if (!ValidIndex(data, i)) return;
    const size_t k = static_cast<size_t>(i);
    const double lo = k > 0 ? data.frames[k - 1].start + data.frames[k - 1].length : 0.0;
    const double hi = k + 1 < data.frames.size() ? data.frames[k + 1].start : source_len;
    const double s = std::min(std::max(start, lo), hi);
    const double e = std::min(std::max(start + length, s), hi);
    if (e - s <= kMinLen) return;  // no room: the frame stays as it was
    data.frames[k].start = s;
    data.frames[k].length = e - s;
    ClampOwnFades(data.frames[k]);
    MarkHand(data, i);
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
    // The cut gets the Sound's default fades: the left piece keeps its fade in, the right its fade out.
    a.fade_out = -1.0;
    a.fade_out_shape = 0;
    b.fade_in = -1.0;
    b.fade_in_shape = 0;
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
        k = std::min(k, data.divide_values.size() - 1);
        owner = data.divide_values[k];
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

bool Touches(const PartSpan& a, const PartSpan& b)
{
    return std::fabs(a.start + a.length - b.start) <= kTimeEps;
}

double MarginS(double ms)
{
    return std::isfinite(ms) && ms > 0.0 ? ms / 1000.0 : 0.0;
}

// Where the run of touching spans holding raw[i] starts / ends (an excluded zone or the frame's edge
// beyond).
double RunStart(const std::vector<PartSpan>& raw, size_t i)
{
    while (i > 0 && Touches(raw[i - 1], raw[i])) --i;
    return raw[i].start;
}

double RunEnd(const std::vector<PartSpan>& raw, size_t i)
{
    while (i + 1 < raw.size() && Touches(raw[i], raw[i + 1])) ++i;
    return raw[i].start + raw[i].length;
}

// 13-54: the trims PartPieces applies, measured: one entry per raw span (a dropped silent span: length 0
// at its start). 13-54 review: the margins apply at the inner edges only (cuts, excluded zones); a run
// that reaches the variation's own start or end keeps it (the frame already has its pre-roll / tail).
std::vector<PartSpan> MarginsPerRaw(const SoundData& data, int variation, const PeakData& peaks)
{
    std::vector<PartSpan> raw = PartSpansOf(data, variation);
    const SoundFrame&     fr = data.frames[static_cast<size_t>(variation)];
    const double          fs = fr.start, fe = FrameEnd(fr);
    const double          pre = MarginS(data.silence.preroll_ms), tail = MarginS(data.silence.tail_ms);
    const double          thr = data.silence.threshold_db;
    const size_t          n = raw.size();
    // Each span's own sound: its first loud bin's start, its last loud bin's end (inside the span).
    std::vector<double>   a(n, 0.0), b(n, 0.0);
    std::vector<bool>     loud(n, false), keep(n, true);
    std::set<std::string> sounding;  // the parts with a loud span here
    for (size_t i = 0; i < n; ++i) {
        const PartSpan& s = raw[i];
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
    // A silent span of a part that sounds elsewhere in the variation plays nothing (dropped: the silence
    // there is shared like any gap); a part silent all along keeps its spans as they are (a wall).
    for (size_t i = 0; i < n; ++i) keep[i] = loud[i] || sounding.count(raw[i].value_uid) == 0;
    std::vector<PartSpan> out = raw;
    // 13-58 (Antho 2026-10-14, "the pre-roll must butt on the previous zone, without changing its size"):
    // the zones never overlap and never shrink each other, the margins only fill the free space. Each part
    // owns its side of the cut: its pre-roll and tail inside its own span come first (`own`). Only then
    // may a margin cross the cut, into what the neighbour leaves free: ends first, each tail up to the
    // next sounding part's own start (its sound minus the pre-roll its span holds) or a wall's edge; then
    // each pre-roll back to the previous part's end as just set (its sound plus its tail), so it gets what
    // is left of the gap, possibly nothing. Before 13-58 the pre-roll was served first and could reach
    // back over the cut into the previous part's tail.
    std::vector<double> own(n, 0.0);
    for (size_t i = 0; i < n; ++i)
        if (loud[i]) own[i] = std::min(std::max(a[i] - pre, raw[i].start), a[i]);
    for (size_t i = n; i-- > 0;) {
        if (!loud[i]) continue;
        double upper = raw[i].start + raw[i].length;
        bool   inner = false;  // stopped at another part's sound or a wall (else: a zone or the frame's end)
        for (size_t j = i + 1; j < n && Touches(raw[j - 1], raw[j]); ++j) {
            inner = true;
            if (loud[j]) {
                upper = own[j];
                break;
            }
            if (keep[j]) {
                upper = raw[j].start;
                break;
            }
            upper = raw[j].start + raw[j].length;
            inner = false;
        }
        // The variation's own end (no cut, no zone after): kept as the frame has it.
        const double end = !inner && std::fabs(upper - fe) <= kTimeEps ? fe : std::max(std::min(b[i] + tail, upper), b[i]);
        out[i].length = end - out[i].start;
    }
    // Then starts: the pre-roll, back to the previous sounding part's end (or a wall's edge).
    for (size_t i = 0; i < n; ++i) {
        if (!loud[i]) continue;
        const double end = out[i].start + out[i].length;
        double       lower = raw[i].start;
        bool         inner = false;
        for (size_t j = i; j > 0 && Touches(raw[j - 1], raw[j]); --j) {
            const size_t p = j - 1;
            inner = true;
            if (loud[p]) {
                lower = out[p].start + out[p].length;
                break;
            }
            if (keep[p]) {
                lower = raw[p].start + raw[p].length;
                break;
            }
            lower = raw[p].start;
            inner = false;
        }
        // The variation's own start (no cut, no zone before): kept as the frame has it.
        if (!inner && std::fabs(lower - fs) <= kTimeEps) out[i].start = fs;
        else out[i].start = std::min(std::max(a[i] - pre, lower), a[i]);
        out[i].length = end - out[i].start;
    }
    for (size_t i = 0; i < n; ++i)
        if (!keep[i]) out[i].length = 0.0;
    return out;
}

// 13-54: the stored trims applied to the raw spans, one entry per raw span (length 0 = dropped); false
// when they do not fit (another span count, out of their run, overlapping): stale.
bool TrimmedPerRaw(const SoundData& data, int variation, const std::vector<PartSpan>& raw, std::vector<PartSpan>* out)
{
    const std::vector<double>& t = data.frames[static_cast<size_t>(variation)].part_trims;
    if (t.size() != raw.size() * 2) return false;
    *out = raw;
    double prev_end = -std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < raw.size(); ++i) {
        const double s = t[2 * i], e = t[2 * i + 1];
        const double rs = raw[i].start, re = raw[i].start + raw[i].length;
        if (!std::isfinite(s) || !std::isfinite(e)) return false;
        if (std::fabs(e - s) <= kTimeEps) {  // dropped
            if (s < rs - kTimeEps || s > re + kTimeEps) return false;
            (*out)[i].length = 0.0;
            continue;
        }
        if (!(e > s) || s < RunStart(raw, i) - kTimeEps || e > RunEnd(raw, i) + kTimeEps || s < prev_end - kTimeEps ||
            !(s < re) || !(e > rs))
            return false;
        (*out)[i].start = s;
        (*out)[i].length = e - s;
        prev_end = e;
    }
    return true;
}

std::vector<PartSpan> WithoutDropped(const std::vector<PartSpan>& spans)
{
    std::vector<PartSpan> out;
    for (const PartSpan& s : spans)
        if (s.length > kTimeEps) out.push_back(s);
    return out;
}

}  // namespace

std::string PartTrimKey(const SoundData& data, int variation, const FileVersion& file)
{
    // The raw spans to the microsecond with their owners (FNV-1a): a moved cut, a zone, another divide
    // makes another key, even when written by a RAV that does not know the trims (it keeps them as read).
    auto us = [](double t) { return std::isfinite(t) ? static_cast<long long>(std::llround(t * 1e6)) : 0LL; };
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](const std::string& text) {
        for (unsigned char c : text) {
            h ^= c;
            h *= 1099511628211ULL;
        }
    };
    if (ValidIndex(data, variation))
        for (const PartSpan& sp : PartSpansOf(data, variation))
            mix(std::to_string(us(sp.start)) + ":" + std::to_string(us(sp.start + sp.length)) + ":" + sp.value_uid + ";");
    char buf[160];
    // The settings in millionths (dB, ms): stable through the record's shortest round-trip numbers.
    // 13-58: led by the margin rule's version ("r2": tails served before pre-rolls); trims measured with
    // the 13-54 rule (no version) are stale and measured again.
    std::snprintf(buf, sizeof(buf), "r2,%lld,%lld,%lld,%016llx|%lld,%llu", us(data.silence.threshold_db), us(data.silence.preroll_ms),
                  us(data.silence.tail_ms), static_cast<unsigned long long>(h), file.time, file.size);
    return buf;
}

bool PartTrimsStale(const SoundData& data, int variation, const FileVersion* file)
{
    if (!ValidIndex(data, variation) || !PartMarginsApply(data)) return false;
    const std::vector<PartSpan> raw = PartSpansOf(data, variation);
    if (raw.empty()) return false;
    std::vector<PartSpan> per;
    if (!TrimmedPerRaw(data, variation, raw, &per)) return true;
    const std::string& have = data.frames[static_cast<size_t>(variation)].part_trim_key;
    const std::string  want = PartTrimKey(data, variation, file ? *file : FileVersion{});
    const size_t       hb = have.find('|'), wb = want.find('|');
    if (hb == std::string::npos || have.compare(0, hb, want, 0, wb) != 0) return true;  // settings or spans
    if (!file || *file == FileVersion{}) return false;
    return have.compare(hb, std::string::npos, want, wb, std::string::npos) != 0;      // another file version
}

std::vector<PartSpan> PartPieces(const SoundData& data, int variation)
{
    std::vector<PartSpan> raw = PartSpansOf(data, variation);
    if (!PartMarginsApply(data) || raw.empty() || PartTrimsStale(data, variation)) return raw;
    std::vector<PartSpan> per;
    return TrimmedPerRaw(data, variation, raw, &per) ? WithoutDropped(per) : raw;
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

std::vector<double> AutoCuts(const PeakData* peaks, const SoundFrame& frame, int parts, bool* unsure)
{
    if (unsure) *unsure = false;
    if (parts < 2) return {};
    if (HasPeaks(peaks)) {
        std::vector<Onset> ons = FindOnsets(*peaks, frame.start, frame.length);
        if (ons.size() >= static_cast<size_t>(parts)) {  // the first onset, then parts - 1 more
            std::vector<Onset> after(ons.begin() + 1, ons.end());
            std::stable_sort(after.begin(), after.end(), [](const Onset& a, const Onset& b) { return a.db > b.db; });
            const bool          q = parts == 2 && after.size() >= 2 && after[0].db - after[1].db < kUnsureDb;
            std::vector<double> cuts;
            for (int k = 0; k < parts - 1; ++k) cuts.push_back(after[static_cast<size_t>(k)].rise - kCutLead);
            if (SaneCuts(frame, cuts)) {
                if (unsure) *unsure = q;
                return cuts;
            }
        }
    }
    std::vector<double> even = EvenCuts(frame, parts);
    SaneCuts(frame, even);
    return even;
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
    double base = spans.front().start;
    if (data.sync_mode != SyncMode::SliceStart && HasPeaks(peaks)) {
        double top = kSilentDb;
        for (const PartSpan& s : spans) {
            long long lo = 0, hi = 0;
            BinRange(*peaks, s.start, s.start + s.length, &lo, &hi);
            for (long long i = lo; i < hi; ++i) top = std::max(top, BinDb(*peaks, i));
        }
        if (top > kSilentDb) {
            // Attack: the first bin reaching the threshold, or 20 dB under the peak when that is higher
            // (never above the peak: a part quieter than the threshold still has an attack).
            const double want = data.sync_mode == SyncMode::Peak
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
    const double off = std::isfinite(data.sync_offset_ms) ? data.sync_offset_ms / 1000.0 : 0.0;
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
        for (const PartSpan& sp : MarginsPerRaw(data, variation, *peaks)) {
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
        return;
    }
    const int n = PartCount(data);
    if (n < 2) {
        f.cuts.clear();
        f.cuts_hand = f.cut_unsure = false;
    } else if (f.cuts_hand && f.cuts.size() == static_cast<size_t>(n - 1) && SaneCuts(f, f.cuts)) {
        f.cut_unsure = false;
    } else if (HasPeaks(peaks)) {
        bool q = false;
        f.cuts = AutoCuts(peaks, f, n, &q);
        f.cuts_hand = false;
        f.cut_unsure = q;
    } else if (!(f.cuts.size() == static_cast<size_t>(n - 1) && SaneCuts(f, f.cuts))) {
        f.cuts = EvenCuts(f, n);
        SaneCuts(f, f.cuts);
        f.cuts_hand = f.cut_unsure = false;
    }
    RefreshTrims(data, variation, peaks);  // 13-54: before the syncs, measured over the trimmed parts
    const std::vector<double> old = f.syncs;
    std::vector<double>       syncs(static_cast<size_t>(n), -1.0);
    const bool                measurable = data.sync_mode == SyncMode::SliceStart || HasPeaks(peaks);
    for (int k = 0; k < n; ++k) {
        const std::vector<PartSpan> spans = SpansOfPart(data, variation, k);
        if (spans.empty()) continue;
        double s = HandSyncOf(data, variation, k, spans);  // 13-54 review: by its raw owner
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
    if (data.sync_mode != SyncMode::SliceStart && !HasPeaks(peaks)) return false;
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
    for (SoundFrame& f : now) {
        f.hand_syncs.clear();
        if (with_cuts && f.cuts_hand) {
            f.cuts.clear();
            f.cuts_hand = f.cut_unsure = false;
        }
    }
    for (const SoundFrame& o : old) {
        for (double h : o.hand_syncs) {
            SoundFrame* f = holder(h);
            if (f && std::find(f->hand_syncs.begin(), f->hand_syncs.end(), h) == f->hand_syncs.end()) f->hand_syncs.push_back(h);
        }
        if (!with_cuts || !o.cuts_hand || o.cuts.empty()) continue;
        SoundFrame* f = holder(o.cuts.front());
        bool        all = f != nullptr;
        for (double c : o.cuts) all = all && holder(c) == f;
        if (all && !f->cuts_hand) {
            f->cuts = o.cuts;
            f->cuts_hand = true;
            f->cut_unsure = false;
        }
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
    }
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
    for (SoundFrame& f : data.frames) f.part_trims.clear();  // 13-54: the spans changed
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
    for (SoundFrame& f : data.frames) f.part_trims.clear();  // 13-54: the spans changed
    return true;
}

bool MoveCut(SoundData& data, int variation, int k, double t)
{
    if (!ValidIndex(data, variation) || !std::isfinite(t)) return false;
    SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    if (k < 0 || static_cast<size_t>(k) >= f.cuts.size()) return false;
    const size_t i = static_cast<size_t>(k);
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
    if (!f.cuts_hand) return false;
    f.cuts.clear();
    f.cuts_hand = f.cut_unsure = false;
    f.part_trims.clear();
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
        if (std::isfinite(pk) && pk + FrameGainDb(f, sp.value_uid) > 1e-9) return true;
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
        if (a[i].start != b[i].start || a[i].length != b[i].length || a[i].on != b[i].on || a[i].cuts != b[i].cuts) return false;
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

}  // namespace rav
