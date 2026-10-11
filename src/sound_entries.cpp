// SPDX-License-Identifier: MIT
//
// See sound_entries.h. Pure: no REAPER, no file I/O (13-3, spec D1-D8).

#include "sound_entries.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
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

std::vector<SoundFrame> DetectFrames(const EntryFacts& facts, DetectMode mode, const SilenceParams& silence,
                                     const PeakData* peaks)
{
    std::vector<SoundFrame> out;
    switch (mode) {
    case DetectMode::Takes:
        for (const SoundFrame& t : facts.takes) {
            SoundFrame f;
            f.start = t.start;
            f.length = t.length > 0.0 ? t.length : facts.item_len;
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
    const double we = std::min(facts.start_offs + facts.item_len, facts.source_len);
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
    data.frames = detected;
    data.edited = false;
}

double FramePeakDb(const PeakData& peaks, const SoundFrame& frame)
{
    if (peaks.rate <= 0.0 || peaks.max_abs.empty()) return NegInf();
    // 1e-9 guards against 0.6 * 10 = 6.0000000001 reaching one peak past the frame.
    const double a = std::floor(frame.start * peaks.rate + 1e-9);
    const double b = std::ceil((frame.start + frame.length) * peaks.rate - 1e-9);
    const long long n = static_cast<long long>(peaks.max_abs.size());
    const long long lo = std::max(0LL, static_cast<long long>(a));
    const long long hi = std::min(n, static_cast<long long>(b));
    float mx = 0.0f;
    for (long long i = lo; i < hi; ++i) mx = std::max(mx, peaks.max_abs[static_cast<size_t>(i)]);
    if (mx <= 0.0f) return NegInf();
    return 20.0 * std::log10(static_cast<double>(mx));
}

void NormalizeFrames(std::vector<SoundFrame>& frames, const PeakData& peaks, double target_db, bool whole,
                     const std::vector<int>& selection)
{
    std::vector<int> set;
    if (selection.empty()) {
        for (size_t i = 0; i < frames.size(); ++i)
            if (frames[i].on) set.push_back(static_cast<int>(i));
    } else {
        for (int i : selection)
            if (i >= 0 && i < static_cast<int>(frames.size()) && frames[static_cast<size_t>(i)].on) set.push_back(i);
    }
    if (whole) {
        double top = NegInf();
        for (int i : set) top = std::max(top, FramePeakDb(peaks, frames[static_cast<size_t>(i)]));
        if (!std::isfinite(top)) return;
        for (int i : set)
            if (std::isfinite(FramePeakDb(peaks, frames[static_cast<size_t>(i)]))) {
                frames[static_cast<size_t>(i)].db = target_db - top;
                frames[static_cast<size_t>(i)].hand = true;
            }
    } else {
        for (int i : set) {
            const double p = FramePeakDb(peaks, frames[static_cast<size_t>(i)]);
            if (std::isfinite(p)) {
                frames[static_cast<size_t>(i)].db = target_db - p;
                frames[static_cast<size_t>(i)].hand = true;
            }
        }
    }
}

DetectMode DefaultDetectMode(const EntryFacts& facts, bool has_peaks)
{
    if (facts.takes.size() >= 2) return DetectMode::Takes;
    if (facts.slices.size() >= 2) return DetectMode::TakeMarkers;
    if (has_peaks) return DetectMode::Silence;
    return DetectMode::Whole;
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
    data.frames[static_cast<size_t>(i)].db = db;
    MarkHand(data, i);
}

}  // namespace rav
