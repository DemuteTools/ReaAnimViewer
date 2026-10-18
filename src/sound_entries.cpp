// SPDX-License-Identifier: MIT
//
// See sound_entries.h. Pure: no REAPER, no file I/O (13-3, spec D1-D8).

#include "sound_entries.h"

#include "sound_record.h"  // FindGroup
#include "sound_zones.h"   // 13-62h: a divided Sound's zones

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
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
    case DetectMode::TakeMarkers: {
        // 13-68 (Antho's NVK items, 2026-10-18): an item that plays a short window around ONE take marker
        // (marker = the variation's sync point, e.g. 58 ms in, 208 ms long) says what a variation is: that
        // same window around each marker. Marker-to-marker slices gave 2.4 s copies with the next variation's
        // sound at their end. An item playing a whole slice (or several markers) keeps the slices.
        int in_window = -1, n_in = 0;
        for (size_t k = 0; k < facts.slices.size(); ++k)
            if (facts.slices[k].start >= facts.start_offs && facts.slices[k].start < facts.start_offs + src_len) {
                in_window = static_cast<int>(k);
                ++n_in;
            }
        if (n_in == 1 && src_len > kMinLen) {
            const Slice& cur = facts.slices[static_cast<size_t>(in_window)];
            const double cur_len = cur.length < 0.0 ? facts.source_len - cur.start : cur.length;
            if (facts.start_offs + src_len < cur.start + cur_len - kMinLen) {
                const double pre = cur.start - facts.start_offs;
                for (const Slice& s : facts.slices) {
                    SoundFrame f;
                    f.start = s.start - pre;
                    f.length = src_len;
                    out.push_back(f);
                }
                ClampFrames(out, facts.source_len);
                if (out.size() >= 2) return out;
                out.clear();
            }
        }
        for (const Slice& s : facts.slices) {
            SoundFrame f;
            f.start = s.start;
            f.length = s.length < 0.0 ? facts.source_len - s.start : s.length;
            out.push_back(f);
        }
        ClampFrames(out, facts.source_len);
        if (out.size() >= 2) return out;
        break;
    }
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
    CarrySides(data.frames, old, data.side_pattern, data.sides, data.mode == DetectMode::Takes);  // 13-73: sides kept by time
    CarryHandMarks(data.frames, old, false);  // A7: hand sync points survive, hand cuts go
    for (SoundFrame& f : data.frames) f.owners.clear();  // 13-62: and the owners with them
    data.zones.erase(std::remove_if(data.zones.begin(), data.zones.end(), [](const SoundZone& z) { return !z.to.empty(); }),
                     data.zones.end());  // excluded zones stay, "-> value" ones go
    data.edited = false;
}

bool SwitchDetectMode(SoundData& data, DetectMode mode, const std::vector<SoundFrame>& detected)
{
    if (data.mode == mode) return false;
    std::vector<SoundFrame> old = data.frames;
    const DetectMode        old_mode = data.mode;
    const bool              old_edited = data.edited;
    if (!data.parked.empty() && data.parked_mode == mode) {
        data.frames = data.parked;
        data.edited = data.parked_edited;
    } else {
        data.frames = detected;
        data.edited = false;
        CarryHandMarks(data.frames, old, false);  // A7: hand sync points kept while their variation exists
        CarrySides(data.frames, old, data.side_pattern, data.sides, mode == DetectMode::Takes && old_mode == DetectMode::Takes);  // 13-73
    }
    data.mode = mode;
    data.parked = std::move(old);
    data.parked_mode = old_mode;
    data.parked_edited = old_edited;
    return true;
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

void SetDefaultFades(SoundData& data, double in, double out)
{
    data.fade_in = std::isfinite(in) ? std::max(in, 0.0) : kDefaultFadeIn;
    data.fade_out = std::isfinite(out) ? std::max(out, 0.0) : kDefaultFadeOut;
}

void SetFrameOn(SoundData& data, int i, bool on)
{
    if (!ValidIndex(data, i)) return;
    data.frames[static_cast<size_t>(i)].on = on;
    MarkHand(data, i);
    // 13-62h: its zones come back into the model: laid out with the others (zones grown into its stretch meanwhile
    // never overlap them).
    if (on && UsesZones(data)) {
        if (data.frames[static_cast<size_t>(i)].zones.empty()) ProposeZones(data, i, nullptr);
        RelayoutZones(data, nullptr);
    }
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

constexpr double kMinPiece = 0.004;  // 13-62: a comped piece is at least 4 ms

// 13-62h: the variation is edited as zones (a divided Sound, not Takes mode, with zones): the queries below read
// its zones, never its cuts / owners / Sound-level zones.
bool ZonedFrame(const SoundData& data, int variation)
{
    return variation >= 0 && static_cast<size_t>(variation) < data.frames.size() && UsesZones(data) &&
           !data.frames[static_cast<size_t>(variation)].zones.empty();
}

}  // namespace

// 13-62: the frame's owners give each piece between its cuts a divide value.
bool OwnersSane(const SoundData& data, const SoundFrame& f)
{
    if (f.owners.empty() || f.owners.size() != f.cuts.size() + 1) return false;
    for (const std::string& o : f.owners)
        if (PartIndex(data, o) < 0) return false;
    return true;
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
    if (!IsDivided(data)) return value_uid == kWholePart ? 0 : -1;  // 13-62i: the implicit part
    if (value_uid.empty()) return -1;
    for (size_t k = 0; k < data.divide_values.size(); ++k)
        if (data.divide_values[k] == value_uid) return static_cast<int>(k);
    return -1;
}

std::vector<std::string> PartKeys(const SoundData& data)
{
    return IsDivided(data) ? data.divide_values : std::vector<std::string>{kWholePart};
}

bool PartAt(const SoundData& data, int variation, double t, std::string* value)
{
    if (!ValidIndex(data, variation)) return false;
    const SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    if (ZonedFrame(data, variation)) {  // 13-62h: the part zone holding t
        if (!f.on) return false;
        for (const EditZone& z : f.zones)
            if (!z.IsGap() && t >= z.a && t < z.b) {
                if (value) *value = z.part;
                return true;
            }
        return false;
    }
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
    if (ZonedFrame(data, variation)) {  // 13-62h: its part zones, in time order (Gaps left out)
        for (const EditZone& z : f.zones)
            if (!z.IsGap()) out.push_back({z.part, z.a, z.b - z.a});
        return out;
    }
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
    const double          thr = data.silence.threshold_db;
    const size_t          n = raw.size();
    // 13-62: each span's own part's pre-roll and tail (the Sound's when the part has none of its own).
    std::vector<double>   pre(n, 0.0), tail(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        pre[i] = MarginS(PartPrerollMs(data, raw[i].value_uid));
        tail[i] = MarginS(PartTailMs(data, raw[i].value_uid));
    }
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
        if (loud[i]) own[i] = std::min(std::max(a[i] - pre[i], raw[i].start), a[i]);
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
        const double end = !inner && std::fabs(upper - fe) <= kTimeEps ? fe : std::max(std::min(b[i] + tail[i], upper), b[i]);
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
        else out[i].start = std::min(std::max(a[i] - pre[i], lower), a[i]);
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
    std::snprintf(buf, sizeof(buf), "r2,%lld,%lld,%lld,%016llx", us(data.silence.threshold_db), us(data.silence.preroll_ms),
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
    if (!ValidIndex(data, variation) || !PartMarginsApply(data) || ZonedFrame(data, variation)) return false;
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
    if (ZonedFrame(data, variation)) return raw;  // 13-62h: the zones are what is placed
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

namespace {

// Fix 2026-10-19 (the snap offset a few ms early): Attack / Peak measured on the file's samples over the spans
// (PeakData::samples): Peak = the frame of the largest |sample| (the earliest on a tie), Attack = the first frame
// reaching max(threshold, that peak - 20 dB), capped at the peak, as on the bins. False when the samples cannot be
// read (no reader, a span too long to read at once, silence): the bins decide.
constexpr double kFineSyncMaxSeconds = 30.0;

bool FineSync(const SoundData& data, const PeakData& peaks, SyncMode mode, const std::vector<PartSpan>& spans, double* out)
{
    if (!peaks.samples) return false;
    double total = 0.0;
    for (const PartSpan& s : spans) total += std::max(0.0, s.length);
    if (!(total > 0.0) || total > kFineSyncMaxSeconds) return false;
    std::vector<std::shared_ptr<const SampleMags>> got;
    float                                          top = 0.0f;
    for (const PartSpan& s : spans) {
        std::shared_ptr<const SampleMags> m = s.length > 0.0 ? peaks.samples(s.start, s.start + s.length) : nullptr;
        if (!m || !(m->sr > 0.0)) return false;
        for (float v : m->mag) top = std::isfinite(v) && v > top ? v : top;
        got.push_back(std::move(m));
    }
    if (!(top > 0.0f) || 20.0 * std::log10(static_cast<double>(top)) <= kSilentDb) return false;
    const double top_db = 20.0 * std::log10(static_cast<double>(top));
    const double want_db = mode == SyncMode::Peak ? top_db : std::min(top_db, std::max(data.silence.threshold_db, top_db - kAttackBelowPeakDb));
    // Peak: the very frame holding `top`; Attack: the first at the level (a float rounding never misses the peak).
    const float want = mode == SyncMode::Peak ? top : std::min(top, static_cast<float>(std::pow(10.0, want_db / 20.0)));
    for (size_t k = 0; k < spans.size(); ++k) {
        const SampleMags& m = *got[k];
        const double      a = spans[k].start, b = spans[k].start + spans[k].length;
        for (size_t i = 0; i < m.mag.size(); ++i) {
            const double t = m.t0 + static_cast<double>(i) / m.sr;
            if (t < a - 1e-9 || t >= b) continue;  // only the frames inside the span
            if (m.mag[i] >= want) {
                *out = t;
                return true;
            }
        }
    }
    return false;
}

}  // namespace

double AutoSync(const SoundData& data, const PeakData* peaks, const std::vector<PartSpan>& spans, bool* fine, bool samples)
{
    if (fine) *fine = false;
    if (spans.empty()) return -1.0;
    double         base = spans.front().start;
    const SyncMode mode = PartSyncMode(data, spans.front().value_uid);  // 13-62: the part's own, else the Sound's
    double         t = 0.0;
    if (mode != SyncMode::SliceStart && peaks && samples && FineSync(data, *peaks, mode, spans, &t)) {
        base = t;
        if (fine) *fine = true;
    } else if (mode != SyncMode::SliceStart && HasPeaks(peaks)) {
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
    // 13-62h / 13-62i: every Sound is laid out as zones (proposed when it has none), laid out again; no cuts, no
    // syncs (Takes mode too: each take on its own source, its own timeline; the peaks given are that take's).
    if (f.zones.empty()) ProposeZones(data, variation, peaks);
    f.cuts.clear();
    f.cuts_hand = f.cut_unsure = false;
    f.syncs.clear();
    f.part_trims.clear();
    f.part_trim_key.clear();
    f.owners.clear();
    RelayoutZones(data, peaks, 0.0, ZoneTimeline(data, variation));
}

void RefreshParts(SoundData& data, const PeakData* peaks, int tl)
{
    // 13-62h / 13-62i: every variation with no zone gets them, then one layout (the old model's keys cleared).
    for (SoundFrame& f : data.frames) {
        f.cuts.clear();
        f.cuts_hand = f.cut_unsure = false;
        f.syncs.clear();
        f.part_trims.clear();
        f.part_trim_key.clear();
        f.owners.clear();
    }
    EnsureZones(data, peaks, 0.0, tl);
}

bool SyncsStale(const SoundData& data, int variation)
{
    if (!ValidIndex(data, variation) || data.mode == DetectMode::Takes || ZonedFrame(data, variation)) return false;
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
    if (!ValidIndex(data, variation) || data.mode == DetectMode::Takes || ZonedFrame(data, variation)) return false;
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
    for (SoundFrame& f : now) {
        f.hand_syncs.clear();
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

namespace {

// 13-62i (Divide by -> None): the parts' own settings go; what they gave a zone is written into the zone first, so
// Populate places the same: a fade taken from its part becomes the zone's own; its snap when its part had its own Sync
// point / Offset (as its measured snap); an automatic edge its part's own Pre-roll / Tail placed stays where it is (by hand). (A part's own
// Mono has no per-zone home: the Sound's applies.)
void BakePartsIntoZones(SoundData& d)
{
    std::vector<std::pair<ZoneRef, EditZone>> baked;
    for (size_t f = 0; f < d.frames.size(); ++f)
        for (size_t k = 0; k < d.frames[f].zones.size(); ++k) {
            const EditZone&     z = d.frames[f].zones[k];
            const PartSettings* p = z.IsGap() ? nullptr : PartSettingsOf(d, z.part);
            if (!p) continue;
            const ZoneRef r{static_cast<int>(f), static_cast<int>(k)};
            EditZone      nz = z;
            if (ZoneFade(d, r, -1).src == FadeSrc::Part) nz.fade_in = ZoneFade(d, r, -1).s;
            if (ZoneFade(d, r, 1).src == FadeSrc::Part) nz.fade_out = ZoneFade(d, r, 1).s;
            if (!z.snap && (p->sync_mode || p->sync_offset_ms)) {
                // As its measured snap (auto_snap, held while its edges and the Sound's Sync point stay): an own snap
                // would make it the event's anchor. A Sound in Slice start reads no measure: its own snap then.
                const double t = ZoneSnap(d, r, nullptr).t;
                if (d.sync_mode == SyncMode::SliceStart) {
                    nz.snap = t;
                } else {
                    nz.auto_snap.set = true;
                    nz.auto_snap.t = t;
                    nz.auto_snap.a = z.a;
                    nz.auto_snap.b = z.b;
                    nz.auto_snap.mode = static_cast<int>(d.sync_mode);
                    nz.auto_snap.thr = d.silence.threshold_db;
                    nz.auto_snap.off = std::isfinite(d.sync_offset_ms) ? d.sync_offset_ms : 0.0;
                }
            }
            if (p->preroll_ms && !ZoneOf(d, GapBefore(d, r))) nz.a_hand = true;
            if (p->tail_ms && !ZoneOf(d, GapAfter(d, r))) nz.b_hand = true;
            baked.push_back({r, nz});
        }
    for (const auto& b : baked) d.frames[static_cast<size_t>(b.first.frame)].zones[static_cast<size_t>(b.first.index)] = b.second;
}

}  // namespace

bool SetDivide(SoundData& data, const std::string& group, const std::vector<std::string>& values)
{
    const std::vector<std::string> vals = group.empty() ? std::vector<std::string>() : values;
    if (data.divide_group == group && data.divide_values == vals) return false;
    if (vals.empty()) BakePartsIntoZones(data);  // 13-62i: before the parts' settings go
    data.divide_group = group;
    data.divide_values = vals;
    const bool whole = vals.empty();
    for (SoundFrame& f : data.frames) {
        // 13-62i: Divide by None: the zones stay where they are (their edges, fades, snaps, gains and Gaps), all of the
        // implicit part.
        if (whole && !f.zones.empty()) {
            for (EditZone& z : f.zones)
                if (!z.IsGap()) z.part = kWholePart;
        } else if (!f.zones.empty()) {
            // 13-62h: the zones go with the old division (RefreshParts proposes new ones); the variation keeps their
            // extent and the first part zone's gain.
            for (const EditZone& z : f.zones)
                if (!z.IsGap()) {
                    f.db = z.db;
                    break;
                }
            f.zones.clear();
        }
        f.cuts.clear();
        f.cuts_hand = f.cut_unsure = false;
        f.syncs.clear();
        f.part_trims.clear();  // 13-54: measured again with the new parts
        f.owners.clear();      // 13-62 (decision 8)
    }
    data.parts.clear();  // 13-62 (decision 8): the parts' own settings go with the old division
    data.parked.clear();  // 13-67b: the parked mode's zones belong to the old division
    data.parked_edited = false;
    data.fader_db.reset();  // 13-62i: and the implicit part's fader (a view of what it applied)
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
    if (value.empty() || value == kWholePart) return nullptr;  // 13-62i: the implicit part's settings are the Sound's
    for (PartSettings& p : data.parts)
        if (p.value == value) return &p;
    return nullptr;
}

int OwnCount(const PartSettings& p)
{
    // 13-62h: fader_db is no setting (a view of what the lane fader applied): not counted, but it keeps the entry.
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
                                    [](const PartSettings& p) { return PartSettingsEmpty(p); }),
                     data.parts.end());
}

bool Finite(const std::optional<double>& v)
{
    return v && std::isfinite(*v);
}

}  // namespace

bool PartSettingsEmpty(const PartSettings& p)
{
    return OwnCount(p) == 0 && p.kept.empty() && !p.fader_db;
}

void PruneEmptyParts(SoundData& data)
{
    PruneParts(data);
}

const PartSettings* PartSettingsOf(const SoundData& data, const std::string& value)
{
    if (value.empty() || value == kWholePart) return nullptr;  // 13-62i: the implicit part's settings are the Sound's
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
    if (value == kWholePart) {  // 13-62i: the implicit part's settings are the Sound's own
        bool syncs = false, trims = false;
        if (Finite(patch.preroll_ms)) {
            data.silence.preroll_ms = std::max(*patch.preroll_ms, 0.0);
            trims = true;
        }
        if (Finite(patch.tail_ms)) {
            data.silence.tail_ms = std::max(*patch.tail_ms, 0.0);
            trims = true;
        }
        if (patch.sync_mode) {
            data.sync_mode = *patch.sync_mode;
            syncs = true;
        }
        if (Finite(patch.sync_offset_ms)) {
            data.sync_offset_ms = *patch.sync_offset_ms;
            syncs = true;
        }
        if (patch.mono) data.mono = *patch.mono;
        if (Finite(patch.fade_in)) data.fade_in = std::max(*patch.fade_in, 0.0);
        if (Finite(patch.fade_out)) data.fade_out = std::max(*patch.fade_out, 0.0);
        // Its gain is its fader (what it applies to every zone of the Sound). Its Normalize mode / target are the
        // user's Level settings (C1: not the Sound's data): nothing here.
        if (Finite(patch.gain_db)) SetPartFader(data, kWholePart, *patch.gain_db);
        Remeasure(data, syncs, trims);
        return;
    }
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

namespace {

// 13-62i: the implicit part's field back to the Sound's default.
void ResetWholeField(SoundData& data, PartField field)
{
    const SoundData def;
    switch (field) {
    case PartField::Preroll: data.silence.preroll_ms = def.silence.preroll_ms; break;
    case PartField::Tail: data.silence.tail_ms = def.silence.tail_ms; break;
    case PartField::SyncMode: data.sync_mode = def.sync_mode; break;
    case PartField::SyncOffset: data.sync_offset_ms = def.sync_offset_ms; break;
    case PartField::Gain: SetPartFader(data, kWholePart, 0.0); break;
    case PartField::Mono: data.mono = def.mono; break;
    case PartField::FadeIn: data.fade_in = def.fade_in; break;
    case PartField::FadeOut: data.fade_out = def.fade_out; break;
    case PartField::NormMode:
    case PartField::NormTarget: break;  // the user's Level settings (C1)
    }
    if (PartFieldRemeasures(field)) Remeasure(data, field == PartField::SyncMode || field == PartField::SyncOffset,
                                              field == PartField::Preroll || field == PartField::Tail);
}

}  // namespace

void ResetPartSetting(SoundData& data, const std::string& value, PartField field)
{
    if (value == kWholePart) {
        ResetWholeField(data, field);
        return;
    }
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
    if (value == kWholePart) {  // 13-62i: every setting of the Sound back to its default (the fader stays, as a part's)
        for (PartField f : {PartField::Preroll, PartField::Tail, PartField::SyncMode, PartField::SyncOffset, PartField::Mono,
                            PartField::FadeIn, PartField::FadeOut})
            ResetWholeField(data, f);
        return;
    }
    PartSettings* p = FindPart(data, value);
    if (!p) return;
    const bool syncs = p->sync_mode || p->sync_offset_ms, trims = p->preroll_ms || p->tail_ms;
    // 13-62h review: only the settings go back to the Sound's; the lane fader (what it applied to the zones,
    // fader_db) and the unknown fields stay (PruneParts drops the entry when nothing is left).
    PartSettings keep;
    keep.value = p->value;
    keep.kept = p->kept;
    keep.fader_db = p->fader_db;
    *p = keep;
    PruneParts(data);
    Remeasure(data, syncs, trims);
}

int OwnSettingCount(const SoundData& data, const std::string& value)
{
    const PartSettings* p = PartSettingsOf(data, value);
    return p ? OwnCount(*p) : 0;
}

// ---- part C: what the Edit window shows and its role gestures -----------------------------------------

int ExcludedZoneCount(const SoundData& data)
{
    int n = 0;
    for (const SoundZone& z : data.zones) n += z.to.empty() && z.length > 0.0 ? 1 : 0;
    return n;
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
    if (!g || g->rav_owned || role.empty() || role == kNoRolesMark) return false;  // 13-68 v3: RAV's values are its own
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

bool NormPartsAvailable(const SoundData& data)
{
    return IsDivided(data);  // 13-62i: a take is laid out as zones too
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
    return "Divided by " + (g ? GroupDisplayName(*g) : std::string("?")) + " (" + vals + ")";
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

// ---- 13-73: Left / Right per variation -------------------------------------------------------------------------

char PatternSide(SidePattern p, int k)
{
    switch (p) {
    case SidePattern::AllLeft: return 'L';
    case SidePattern::AllRight: return 'R';
    case SidePattern::AltRL: return k % 2 == 0 ? 'R' : 'L';
    case SidePattern::AltLR:
    default: return k % 2 == 0 ? 'L' : 'R';
    }
}

char OtherSide(char side) { return side == 'L' ? 'R' : side == 'R' ? 'L' : 0; }

const char* SidePatternLabel(SidePattern p)
{
    switch (p) {
    case SidePattern::AltRL: return "Alternate R L";
    case SidePattern::AllLeft: return "All Left";
    case SidePattern::AllRight: return "All Right";
    case SidePattern::AltLR:
    default: return "Alternate L R";
    }
}

std::vector<char> FrameRuleSides(const std::vector<SoundFrame>& frames, SidePattern p)
{
    std::vector<char> out(frames.size(), 0);
    const bool alt = p == SidePattern::AltLR || p == SidePattern::AltRL;
    // A frame with no side of its own: from the nearest one before it that has one (alternating), else from the first one
    // after it that has one (by parity), else the Pattern by index.
    int next_set = -1;
    for (size_t i = 0; i < frames.size(); ++i)
        if (frames[i].side == 'L' || frames[i].side == 'R') {
            next_set = static_cast<int>(i);
            break;
        }
    for (size_t i = 0; i < frames.size(); ++i) {
        const char own = frames[i].side;
        if (own == 'L' || own == 'R') out[i] = own;
        else if (!alt) out[i] = PatternSide(p, static_cast<int>(i));
        else if (i > 0) out[i] = OtherSide(out[i - 1]);
        else if (next_set > 0) out[i] = (next_set % 2 == 0) ? frames[static_cast<size_t>(next_set)].side : OtherSide(frames[static_cast<size_t>(next_set)].side);
        else out[i] = PatternSide(p, 0);
    }
    return out;
}

std::vector<char> FrameRuleSides(const SoundData& d) { return FrameRuleSides(d.frames, d.side_pattern); }

std::vector<char> FrameSides(const SoundData& d)
{
    std::vector<char> s = FrameRuleSides(d);
    for (size_t i = 0; i < s.size(); ++i)
        if (d.frames[i].side_hand == 'L' || d.frames[i].side_hand == 'R') s[i] = d.frames[i].side_hand;
    return s;
}

bool FrameSideByHand(const SoundData& d, const std::vector<char>& rule, int i)
{
    if (i < 0 || static_cast<size_t>(i) >= d.frames.size() || static_cast<size_t>(i) >= rule.size()) return false;
    const char h = d.frames[static_cast<size_t>(i)].side_hand;
    return (h == 'L' || h == 'R') && h != rule[static_cast<size_t>(i)];
}

bool LockSides(SoundData& d)
{
    const std::vector<char> rule = FrameRuleSides(d);
    bool                    changed = false;
    for (size_t i = 0; i < d.frames.size(); ++i) {
        SoundFrame& f = d.frames[i];
        if (f.side != rule[i]) {
            f.side = rule[i];
            changed = true;
        }
        if (f.side_hand != 0 && f.side_hand != 'L' && f.side_hand != 'R') {
            f.side_hand = 0;
            changed = true;
        }
        if (f.side_hand == rule[i]) {
            f.side_hand = 0;
            changed = true;
        }
    }
    return changed;
}

bool SetSidesOn(SoundData& d, bool on)
{
    if (d.sides == on) return false;
    d.sides = on;
    if (on) LockSides(d);
    return true;
}

bool SetSidePattern(SoundData& d, SidePattern p)
{
    const std::vector<SoundFrame> before = d.frames;
    const SidePattern             was = d.side_pattern;
    d.side_pattern = p;
    for (size_t i = 0; i < d.frames.size(); ++i) d.frames[i].side = PatternSide(p, static_cast<int>(i));
    LockSides(d);
    if (was != p) return true;
    for (size_t i = 0; i < d.frames.size(); ++i)
        if (d.frames[i].side != before[i].side || d.frames[i].side_hand != before[i].side_hand) return true;
    return false;
}

bool SwapSides(SoundData& d)
{
    LockSides(d);
    switch (d.side_pattern) {
    case SidePattern::AltLR: d.side_pattern = SidePattern::AltRL; break;
    case SidePattern::AltRL: d.side_pattern = SidePattern::AltLR; break;
    case SidePattern::AllLeft: d.side_pattern = SidePattern::AllRight; break;
    case SidePattern::AllRight: d.side_pattern = SidePattern::AllLeft; break;
    }
    for (SoundFrame& f : d.frames) {
        f.side = OtherSide(f.side);
        f.side_hand = OtherSide(f.side_hand);
    }
    return true;
}

bool SetFrameSides(SoundData& d, const std::vector<int>& frames, char side)
{
    if (side != 0 && side != 'L' && side != 'R') return false;
    LockSides(d);  // the others keep the side they show now
    const std::vector<char> rule = FrameRuleSides(d);
    bool                    changed = false;
    for (int i : frames) {
        if (i < 0 || static_cast<size_t>(i) >= d.frames.size()) continue;
        const char want = side == rule[static_cast<size_t>(i)] ? 0 : side;
        if (d.frames[static_cast<size_t>(i)].side_hand != want) {
            d.frames[static_cast<size_t>(i)].side_hand = want;
            changed = true;
        }
    }
    return changed;
}

bool ResetHandSides(SoundData& d)
{
    bool changed = false;
    for (SoundFrame& f : d.frames)
        if (f.side_hand != 0) {
            f.side_hand = 0;
            changed = true;
        }
    return changed;
}

SideCounts CountSides(const SoundData& d)
{
    SideCounts              c;
    const std::vector<char> rule = FrameRuleSides(d);
    const std::vector<char> s = FrameSides(d);
    for (size_t i = 0; i < s.size(); ++i) {
        (s[i] == 'L' ? c.left : c.right)++;
        if (FrameSideByHand(d, rule, static_cast<int>(i))) ++c.hand;
    }
    return c;
}

void CarrySides(std::vector<SoundFrame>& now, const std::vector<SoundFrame>& old, SidePattern p, bool sides_on, bool by_index)
{
    bool any = sides_on;
    for (const SoundFrame& f : old) any = any || f.side != 0 || f.side_hand != 0;
    for (SoundFrame& f : now) f.side = f.side_hand = 0;
    if (!any) return;
    if (old.empty()) {  // 13-73 review: a first detection with Left / Right on: the Pattern written at once
        const std::vector<char> first = FrameRuleSides(now, p);
        for (size_t i = 0; i < now.size(); ++i) now[i].side = first[i];
        return;
    }
    const std::vector<char> rule = FrameRuleSides(old, p);
    for (size_t i = 0; i < now.size(); ++i) {
        int best = -1;
        if (by_index) {
            if (i < old.size()) best = static_cast<int>(i);
        } else {
            double      most = 0.0;
            const double a = now[i].start, b = now[i].start + now[i].length;
            for (size_t k = 0; k < old.size(); ++k) {
                const double ov = std::min(b, old[k].start + old[k].length) - std::max(a, old[k].start);
                if (ov > most) {
                    most = ov;
                    best = static_cast<int>(k);
                }
            }
        }
        if (best < 0) continue;  // new: continues the alternation of its neighbours (FrameRuleSides)
        now[i].side = rule[static_cast<size_t>(best)];
        const char h = old[static_cast<size_t>(best)].side_hand;
        now[i].side_hand = h == now[i].side ? 0 : h;
    }
    // Two new frames that took one old frame (it was split) keep its side both; then every side is written so the next
    // re-detection keeps them too.
    const std::vector<char> after = FrameRuleSides(now, p);
    for (size_t i = 0; i < now.size(); ++i) now[i].side = after[i];
}

}  // namespace rav
