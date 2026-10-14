// SPDX-License-Identifier: MIT
//
// The pure rules of the one peak reader (13-55, src/sound_peaks.cpp): which rate a file's peaks are
// read at, the cache key of a path, which waiting file the reader takes next, and how REAPER's peak
// points (max / min per channel) fold into a PeakData. Header-only, host-tested
// (tests/sound_peaks_plan_test.cpp).
//
// Why two rates: REAPER's .reapeaks file keeps its finest peaks at about 400 per second
// (https://www.reaper.fm/sdk/reapeaks.txt, a preference). Read at or below that rate the peaks come
// straight from the peak file, without touching the audio: a long file shows at once. A short file
// (a footstep, an impact) needs finer peaks for its waveform and its silence detection, and reading its
// audio is cheap: it keeps the 1000 peaks/s read from its samples (13-7b's numbers, unchanged).

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sound_model.h"

namespace rav {

constexpr double kPeakHiResRate = 1000.0;      // a short file, read from its samples
constexpr double kPeakFileRate = 380.0;        // a long file, from the peak file (under its ~400/s)
constexpr double kPeakHiResMaxSeconds = 10.0;  // up to this length a file is read at kPeakHiResRate
constexpr double kPeakFreshSeconds = 0.5;      // a waiting file asked this recently goes first
constexpr double kPeakDropSeconds = 2.0;       // a waiting file nobody asked for this long is dropped

// The rate a file of `length` seconds is read at (0 for no length: nothing to read).
inline double ChoosePeakRate(double length)
{
    if (!(length > 0.0)) return 0.0;
    return length <= kPeakHiResMaxSeconds ? kPeakHiResRate : kPeakFileRate;
}

// True when a file of that length is read from its samples (else from REAPER's peaks).
inline bool PeakReadsSamples(double length) { return ChoosePeakRate(length) >= kPeakHiResRate; }

// The bins a file of `length` seconds has at `rate` (ceil + 1, as 13-7b sized them).
inline size_t PeakBins(double length, double rate)
{
    if (!(length > 0.0) || !(rate > 0.0)) return 0;
    return static_cast<size_t>(std::ceil(length * rate)) + 1;
}

// The cache key of a path: one entry per file on Windows, whatever the case or the slashes a caller
// spells it with (the Media Explorer's database and an item's source may differ). Only ASCII letters
// are folded (accepted, 13-55 review): two spellings differing only in the case of an accented
// letter are two entries, which costs a second read of the file, nothing else.
inline std::string PeakCacheKey(const std::string& path)
{
    std::string k = path;
    for (char& c : k) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        else if (c == '/') c = '\\';
    }
    return k;
}

// A file waiting for the reader: the order it was asked in, and when it was last asked.
struct PeakWaiting {
    uint64_t seq = 0;
    double   asked_s = 0.0;
};

struct PeakPick {
    int              next = -1;  // the index to read next, -1 = none
    std::vector<int> drop;       // the indices nobody asks for any more
};

// Which waiting file the reader takes next at `now_s`: the newest one asked recently (the file the
// user just clicked goes before the ones he clicked past), else the newest still wanted. A file not
// asked for kPeakDropSeconds is dropped (asked again later, it waits again). A clock that went back
// keeps the file.
inline PeakPick PickNextPeakJob(const std::vector<PeakWaiting>& w, double now_s)
{
    PeakPick p;
    int      fresh = -1, kept = -1;
    for (size_t i = 0; i < w.size(); ++i) {
        const int    k = static_cast<int>(i);
        const double age = now_s - w[i].asked_s;
        if (age > kPeakDropSeconds) {
            p.drop.push_back(k);
            continue;
        }
        if (kept < 0 || w[i].seq > w[static_cast<size_t>(kept)].seq) kept = k;
        if (age <= kPeakFreshSeconds && (fresh < 0 || w[i].seq > w[static_cast<size_t>(fresh)].seq)) fresh = k;
    }
    p.next = fresh >= 0 ? fresh : kept;
    return p;
}

// Folds `n` peak points of REAPER's GetPeakInfo (`maxv` and, when given, `minv`: `nch` interleaved
// values per point) into bins [first, first + n) of `p`: a channel's peak is max(|max|, |min|).
template <typename Sample>
inline void FoldPeakPoints(PeakData* p, size_t first, const Sample* maxv, const Sample* minv, int n, int nch)
{
    if (!p || !maxv || n <= 0 || nch <= 0) return;
    std::vector<Sample> frame(static_cast<size_t>(nch));
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < nch; ++c) {
            const size_t k = static_cast<size_t>(i) * static_cast<size_t>(nch) + static_cast<size_t>(c);
            Sample       a = maxv[k] < 0 ? -maxv[k] : maxv[k];
            if (minv) {
                const Sample b = minv[k] < 0 ? -minv[k] : minv[k];
                a = a < b ? b : a;
            }
            frame[static_cast<size_t>(c)] = a;
        }
        PeakAddFrame(p, first + static_cast<size_t>(i), frame.data(), nch);
    }
}

// 13-55 review: whether the file the reader is on is given up for another one (its build stops
// between two steps; it waits again if still asked): nobody asked for it these kPeakFreshSeconds
// (the panel that showed it moved on, or a placement holds the UI waiting for another file) while
// another waiting file was asked within them. A clock that went back keeps it.
inline bool PeakAbandonInFlight(double in_flight_asked_s, bool other_fresh, double now_s)
{
    return other_fresh && now_s - in_flight_asked_s > kPeakFreshSeconds;
}

// 13-55 review: which way a read went (debuglog builds say it once per read, with its time).
enum class PeakReadPath { PeakFile, BuiltThenPeakFile, Samples, SamplesFallback };

inline const char* PeakReadPathName(PeakReadPath p)
{
    switch (p) {
    case PeakReadPath::PeakFile: return "peak file";
    case PeakReadPath::BuiltThenPeakFile: return "built then peak file";
    case PeakReadPath::Samples: return "samples (short file)";
    case PeakReadPath::SamplesFallback: return "sample fallback (REAPER gave no peaks)";
    }
    return "?";
}

}  // namespace rav
