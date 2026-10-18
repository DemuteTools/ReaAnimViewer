// SPDX-License-Identifier: MIT
//
// 13-57: a waveform sharp at every zoom. The pure rules of the detail read (src/sound_peaks.cpp):
// a view up to kWaveDetailMaxSpan long is drawn from the file's samples, read in tiles of
// kWaveTileColumns columns at a column rate of a power of two just above the view's pixel rate
// (capped at the file's sample rate: one column per sample). A tile keeps, per column, the signed min
// and max of each series (all channels, L, R, (L + R) / 2, then 13-71: each further channel), so the
// waveform shows its true shape, one lane per channel (WaveLanesFor).
// Tiles are kept in a small LRU (WaveTileLru) keyed on (file, rate, index): panning reuses them,
// a zoom step inside one power of two reuses them, a zoom step across reuses the level next to it
// until its own tiles are read (no flicker back to the coarse peaks).
//
// Then the view is folded into one column per pixel (FoldWaveColumns), each column joined to the one
// before it (JoinWaveColumns, no gaps between steep columns); at a zoom where a pixel is shorter than
// a sample, the samples are drawn as a line instead (WaveColumns::line). The coarse peaks (PeakData,
// max |sample| per bin) give symmetric columns meanwhile (ColumnsFromPeaks).
// Header-only, host-tested (tests/wave_tiles_plan_test.cpp).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "file_change_check.h"
#include "sound_model.h"
#include "sound_peaks_plan.h"

namespace rav {

constexpr int    kWaveTileColumns = 512;
constexpr double kWaveDetailMaxSpan = 60.0;  // a view up to this long (s) is drawn from its samples
constexpr int    kWaveMaxLanes = kPeakMaxLanes;  // 13-71: one lane per channel, at most this many
constexpr int    kWaveSeries = 2 + kWaveMaxLanes;  // 0 all channels, 1 ch 1 (L), 2 ch 2 (R), 3 (L + R) / 2, 4.. ch 3..
constexpr size_t kWaveTileCacheTiles = 384;  // ~16 KB a stereo tile: ~6 MB at most (~40 KB an 8-channel one)

// The series a lane draws: a mono file has series 0 only.
enum WaveSeries { kWaveAll = 0, kWaveLeft = 1, kWaveRight = 2, kWaveSum = 3 };

// 13-71: the series of channel `c` (0-based; c past kWaveMaxLanes folds to c % kWaveMaxLanes).
inline int WaveChannelSeries(int c)
{
    c = ((c % kWaveMaxLanes) + kWaveMaxLanes) % kWaveMaxLanes;
    return c == 0 ? kWaveLeft : c == 1 ? kWaveRight : 2 + c;
}

// 13-71: the series a file of `nch` channels is read into: 1 for a mono file, else 2 + its channels
// (at most kWaveSeries; a stereo file 4, as before).
inline int WaveSeriesCount(int nch)
{
    return nch >= 2 ? 2 + std::min(nch, kWaveMaxLanes) : 1;
}

// 13-71: the channels (lanes) `series` series hold (WaveSeriesCount's inverse; 1 = mono).
inline int WaveSeriesChannels(int series)
{
    return series >= 4 ? std::min(series - 2, kWaveMaxLanes) : 1;
}

// 13-71 (Antho: "the number of channels shows in every waveform"): the lanes a waveform draws, top to
// bottom, and the series each lane reads. Every channel of the file its own lane (stereo = L above R, as
// REAPER), unless the Sound is forced to mono: one lane, the channel (or (L + R) / 2) heard. A mono file
// (or one read without its channels) one lane of series 0. The one rule every waveform drawer uses.
struct WaveLanes {
    int n = 1;
    int series[kWaveMaxLanes] = {kWaveAll};
};

inline WaveLanes WaveLanesFor(MonoMode mono, int series)
{
    WaveLanes w;
    const int ch = WaveSeriesChannels(series);
    if (ch < 2) return w;
    if (mono != MonoMode::Off) {
        w.series[0] = mono == MonoMode::Right ? kWaveRight : mono == MonoMode::Sum ? kWaveSum : kWaveLeft;
        return w;
    }
    w.n = ch;
    for (int l = 0; l < ch; ++l) w.series[l] = WaveChannelSeries(l);
    return w;
}

// The column rate (columns per second) a view of `view_len` seconds over `columns` pixels is read at
// from a file sampled at `sample_rate`: the power of two at or above the pixel rate, capped at the
// sample rate. 0 = no detail read: a view longer than kWaveDetailMaxSpan (the coarse peaks are at least
// as fine as its pixels there), or nothing to read.
inline double WaveTileRate(double view_len, int columns, double sample_rate)
{
    if (!(view_len > 0.0) || columns <= 0 || !(sample_rate > 0.0) || view_len > kWaveDetailMaxSpan) return 0.0;
    const double p = static_cast<double>(columns) / view_len;
    double       r = 1.0;
    while (r < p && r < sample_rate) r *= 2.0;
    return std::min(r, sample_rate);
}

// The source seconds tile `index` covers at `rate`.
inline double WaveTileStart(double rate, int64_t index) { return static_cast<double>(index) * kWaveTileColumns / rate; }
inline double WaveTileEnd(double rate, int64_t index) { return WaveTileStart(rate, index + 1); }

// The tiles [first, last] a view needs at `rate` (clamped to the file's `source_len`); false when none.
inline bool WaveTilesFor(double view_start, double view_len, double rate, double source_len, int64_t* first, int64_t* last)
{
    if (!(rate > 0.0) || !(view_len > 0.0) || !(source_len > 0.0)) return false;
    const double  t0 = std::max(0.0, view_start), t1 = std::min(source_len, view_start + view_len);
    if (!(t1 > t0)) return false;
    const int64_t n = static_cast<int64_t>(std::ceil(source_len * rate / kWaveTileColumns));  // tiles in the file
    int64_t       a = static_cast<int64_t>(std::floor(t0 * rate / kWaveTileColumns));
    int64_t       b = static_cast<int64_t>(std::floor(t1 * rate / kWaveTileColumns));
    a = std::clamp<int64_t>(a, 0, std::max<int64_t>(0, n - 1));
    b = std::clamp<int64_t>(b, a, std::max<int64_t>(0, n - 1));
    *first = a;
    *last = b;
    return true;
}

// One tile: per series, per column, the signed min (lo) and max (hi) of the samples in it. A column
// no sample reached has lo > hi.
struct WaveTile {
    double             rate = 0.0;
    int64_t            index = 0;
    int                series = 1;  // WaveSeriesCount of the file's channels
    std::vector<float> lo[kWaveSeries], hi[kWaveSeries];
    FileVersion        file;  // the file's date + size it was read from
};

inline void WaveTileInit(WaveTile* t, double rate, int64_t index, int nch)
{
    t->rate = rate;
    t->index = index;
    t->series = WaveSeriesCount(nch);
    for (int s = 0; s < kWaveSeries; ++s) {
        if (s < t->series) {
            t->lo[s].assign(kWaveTileColumns, std::numeric_limits<float>::max());
            t->hi[s].assign(kWaveTileColumns, -std::numeric_limits<float>::max());
        } else {
            std::vector<float>().swap(t->lo[s]);
            std::vector<float>().swap(t->hi[s]);
        }
    }
}

// Folds the frame of sample `n` (`nch` interleaved values, the file at `sample_rate`) into its column.
// 13-71: each channel into its own series (WaveChannelSeries; a channel past the tile's folds as
// PeakAddFrame's lanes); the sum = (ch 0 + ch 1) / 2. A sample outside the tile is ignored.
template <typename Sample>
inline void WaveTileAddFrame(WaveTile* t, int64_t n, double sample_rate, const Sample* frame, int nch)
{
    if (!t || nch <= 0 || !(sample_rate > 0.0)) return;
    // Exact for rate == sample_rate (an integer product divided back): one column per sample.
    const int64_t col = static_cast<int64_t>(std::floor(static_cast<double>(n) * t->rate / sample_rate)) -
                        t->index * kWaveTileColumns;
    if (col < 0 || col >= kWaveTileColumns) return;
    const size_t c = static_cast<size_t>(col);
    auto put = [&](int s, float v) {
        if (!(v == v)) return;  // NaN: skipped
        t->lo[s][c] = std::min(t->lo[s][c], v);
        t->hi[s][c] = std::max(t->hi[s][c], v);
    };
    const int nl = WaveSeriesChannels(t->series);
    for (int k = 0; k < nch; ++k) {
        const float v = static_cast<float>(frame[k]);
        put(kWaveAll, v);
        if (t->series > 1) put(WaveChannelSeries(k % nl), v);
    }
    if (t->series > 1) put(kWaveSum, static_cast<float>((frame[0] + frame[1]) * 0.5));
}

// A view folded to one column per pixel. Columns: lo > hi = nothing there. `line`: a pixel is shorter
// than a sample: draw `pt_v` (each series' sample values) at `pt_x` (pixel x from the view's left
// edge, fractional) joined by a line, instead of the columns.
struct WaveColumns {
    int                 columns = 0;
    int                 series = 1;
    bool                detail = false;  // from the samples (signed); false = the coarse peaks (symmetric)
    std::vector<float>  lo[kWaveSeries], hi[kWaveSeries];
    bool                line = false;
    std::vector<double> pt_x;
    std::vector<float>  pt_v[kWaveSeries];
};

inline bool WaveColumnHas(const WaveColumns& w, int s, size_t i)
{
    return s < w.series && i < w.lo[s].size() && w.lo[s][i] <= w.hi[s][i];
}

inline void WaveColumnsInit(WaveColumns* w, int columns, int series)
{
    *w = WaveColumns{};
    w->columns = std::max(0, columns);
    w->series = series;
    for (int s = 0; s < series && s < kWaveSeries; ++s) {
        w->lo[s].assign(static_cast<size_t>(w->columns), std::numeric_limits<float>::max());
        w->hi[s].assign(static_cast<size_t>(w->columns), -std::numeric_limits<float>::max());
    }
}

// The coarse peaks (max |sample| per bin, from 0 s at p.rate) over the view: symmetric columns
// (-max..+max), the sum series as 13-41 drew it ((L + R) / 2 of the lanes' peaks), each lane its
// channel's series (13-71).
inline void ColumnsFromPeaks(const PeakData& p, double view_start, double view_len, int columns, WaveColumns* out)
{
    const int  nl = PeakChannels(p);
    const bool stereo = nl >= 2;
    WaveColumnsInit(out, columns, WaveSeriesCount(nl));
    if (!(p.rate > 0.0) || !(view_len > 0.0) || p.max_abs.empty()) return;
    const int64_t nb = static_cast<int64_t>(p.max_abs.size());
    for (int c = 0; c < out->columns; ++c) {
        const double t0 = view_start + view_len * c / out->columns, t1 = view_start + view_len * (c + 1) / out->columns;
        int64_t      k0 = static_cast<int64_t>(std::floor(t0 * p.rate)), k1 = static_cast<int64_t>(std::floor(t1 * p.rate));
        k1 = std::max(k1, k0 + 1);
        k0 = std::max<int64_t>(0, k0);
        k1 = std::min(k1, nb);
        if (k1 <= k0) continue;
        const size_t i = static_cast<size_t>(c);
        float        m = 0.0f, ml[kWaveMaxLanes] = {};
        for (int64_t k = k0; k < k1; ++k) {
            const size_t b = static_cast<size_t>(k);
            m = std::max(m, p.max_abs[b]);
            if (stereo)
                for (int l = 0; l < nl; ++l) ml[l] = std::max(ml[l], p.lanes[l][b]);
        }
        out->lo[0][i] = -m;
        out->hi[0][i] = m;
        if (stereo) {
            for (int l = 0; l < nl; ++l) {
                const int s = WaveChannelSeries(l);
                out->lo[s][i] = -ml[l];
                out->hi[s][i] = ml[l];
            }
            const float sum = (ml[0] + ml[1]) * 0.5f;
            out->lo[kWaveSum][i] = -sum;
            out->hi[kWaveSum][i] = sum;
        }
    }
}

// The tile of (rate, index), or null when it is not read yet.
using WaveTileLookup = std::function<const WaveTile*(double rate, int64_t index)>;

// The view folded from the tiles: each pixel from the first rate of `rates` (the wanted one first)
// whose tile holding it is read; a pixel no tile holds keeps the `coarse` column (pass it already
// filled by ColumnsFromPeaks, or empty). `out->detail` = every pixel came from a tile at rates[0].
// `series`: the tiles' (WaveSeriesCount of the file's channels); a coarse of other lanes or width is not
// used. `sample_rate`: when rates[0] is it and a pixel is shorter than a sample, and every tile is
// there, the samples are given as a line (`line`). 13-57 review: `source_len` (the file's length, 0 =
// unknown): a pixel entirely before 0 s or past the file's end has nothing to draw and needs no tile
// (the sample line still shows at the file's edges), and a pixel over the end needs only the tiles
// the file has.
inline void FoldWaveColumns(const WaveTileLookup& tile, const std::vector<double>& rates, double sample_rate,
                            int series, double view_start, double view_len, int columns, const WaveColumns& coarse,
                            WaveColumns* out, double source_len = 0.0)
{
    *out = coarse;
    out->detail = false;
    out->line = false;
    if (columns <= 0 || !(view_len > 0.0) || rates.empty()) return;
    if (out->columns != columns || out->series != series) WaveColumnsInit(out, columns, series);
    bool all_wanted = true;
    for (int c = 0; c < columns; ++c) {
        const double t0 = view_start + view_len * c / columns, t1 = view_start + view_len * (c + 1) / columns;
        if (t1 <= 0.0 || (source_len > 0.0 && t0 >= source_len)) {
            const size_t i = static_cast<size_t>(c);  // outside the file: nothing there, nothing to wait for
            for (int s = 0; s < out->series && s < kWaveSeries; ++s) {
                out->lo[s][i] = std::numeric_limits<float>::max();
                out->hi[s][i] = -std::numeric_limits<float>::max();
            }
            continue;
        }
        bool         done = false;
        for (size_t r = 0; r < rates.size() && !done; ++r) {
            const double rate = rates[r];
            if (!(rate > 0.0)) continue;
            int64_t k0 = static_cast<int64_t>(std::floor(t0 * rate)), k1 = static_cast<int64_t>(std::floor(t1 * rate));
            k1 = std::max(k1, k0 + 1);
            if (k0 < 0) k0 = 0;
            if (source_len > 0.0) k1 = std::min(k1, std::max<int64_t>(k0 + 1, static_cast<int64_t>(std::ceil(source_len * rate))));
            if (k1 <= k0) continue;
            // Every tile the pixel spans must be read for this rate to give it.
            const int64_t ta = k0 / kWaveTileColumns, tb = (k1 - 1) / kWaveTileColumns;
            bool          have = true;
            for (int64_t ti = ta; ti <= tb && have; ++ti) have = tile(rate, ti) != nullptr;
            if (!have) continue;
            const int    ns = std::min(series, tile(rate, ta)->series);
            const size_t i = static_cast<size_t>(c);
            for (int s = 0; s < ns; ++s) {
                out->lo[s][i] = std::numeric_limits<float>::max();
                out->hi[s][i] = -std::numeric_limits<float>::max();
            }
            for (int64_t k = k0; k < k1; ++k) {
                const WaveTile* t = tile(rate, k / kWaveTileColumns);
                const size_t    j = static_cast<size_t>(k % kWaveTileColumns);
                for (int s = 0; s < ns; ++s) {
                    out->lo[s][i] = std::min(out->lo[s][i], t->lo[s][j]);
                    out->hi[s][i] = std::max(out->hi[s][i], t->hi[s][j]);
                }
            }
            done = true;
            if (r != 0) all_wanted = false;
        }
        if (!done) all_wanted = false;
    }
    out->detail = all_wanted;
    // A pixel shorter than a sample: the samples as a line (only from a complete sample-rate read).
    const double px_rate = static_cast<double>(columns) / view_len;
    if (all_wanted && sample_rate > 0.0 && rates[0] >= sample_rate && px_rate > sample_rate) {
        const int64_t n0 = std::max<int64_t>(0, static_cast<int64_t>(std::floor(view_start * sample_rate)) - 1);
        const int64_t n1 = static_cast<int64_t>(std::ceil((view_start + view_len) * sample_rate)) + 1;
        for (int64_t n = n0; n <= n1; ++n) {
            const WaveTile* t = tile(rates[0], n / kWaveTileColumns);
            if (!t) continue;
            const size_t j = static_cast<size_t>(n % kWaveTileColumns);
            if (!(t->lo[0][j] <= t->hi[0][j])) continue;  // past the file's end
            out->pt_x.push_back((static_cast<double>(n) / sample_rate - view_start) / view_len * columns);
            for (int s = 0; s < series; ++s) out->pt_v[s].push_back(s < t->series ? t->lo[s][j] : t->lo[0][j]);
        }
        out->line = out->pt_x.size() >= 2;
    }
}

// Each column reaches the one before it (its range is stretched up or down to touch it), so a steep
// waveform has no gap between two columns.
inline void JoinWaveColumns(WaveColumns* w)
{
    for (int s = 0; s < w->series && s < kWaveSeries; ++s) {
        std::vector<float>& lo = w->lo[s];
        std::vector<float>& hi = w->hi[s];
        float pl = 0.0f, ph = 0.0f;
        bool  prev = false;
        for (size_t i = 0; i < lo.size(); ++i) {
            if (!(lo[i] <= hi[i])) {
                prev = false;
                continue;
            }
            const float l = lo[i], h = hi[i];
            if (prev) {
                if (lo[i] > ph) lo[i] = ph;
                if (hi[i] < pl) hi[i] = pl;
            }
            pl = l;
            ph = h;
            prev = true;
        }
    }
}

// The rates to try for a view read at `rate` (the wanted one, then the levels next to it: a zoom
// step shows the level it came from until its own tiles are read), each at most `sample_rate`.
inline std::vector<double> WaveFallbackRates(double rate, double sample_rate)
{
    std::vector<double> r;
    if (!(rate > 0.0)) return r;
    auto add = [&](double v) {
        v = std::min(v, sample_rate);
        if (v > 0.0 && std::find(r.begin(), r.end(), v) == r.end()) r.push_back(v);
    };
    add(rate);
    add(rate * 2.0);
    add(rate / 2.0);
    add(rate * 4.0);
    add(rate / 4.0);
    add(rate / 8.0);
    // The sample-rate level sits at the sample rate, not a power of two: its neighbours are.
    if (rate >= sample_rate) {
        double p = 1.0;
        while (p * 2.0 < sample_rate) p *= 2.0;
        add(p);
        add(p / 2.0);
    }
    return r;
}

// A small LRU of tiles keyed on (file key, rate, index). 13-57 review: one map per file (a lookup
// copies no path), and a generation bumped whenever a tile comes or goes (a waveform built from the
// tiles is built again only then).
struct WaveTileSlot {
    WaveTile tile;
    uint64_t used = 0;
};

class WaveTileLru {
public:
    using FileTiles = std::map<std::pair<double, int64_t>, WaveTileSlot>;
    explicit WaveTileLru(size_t capacity = kWaveTileCacheTiles) : cap_(capacity) {}
    // The tiles of one file (null when it has none): several of its tiles for one path lookup.
    FileTiles* Of(const std::string& file)
    {
        auto f = m_.find(file);
        return f == m_.end() ? nullptr : &f->second;
    }
    const WaveTile* Get(FileTiles& tiles, double rate, int64_t index)
    {
        auto it = tiles.find(std::make_pair(rate, index));
        if (it == tiles.end()) return nullptr;
        it->second.used = ++tick_;
        return &it->second.tile;
    }
    const WaveTile* Get(const std::string& file, double rate, int64_t index)
    {
        FileTiles* f = Of(file);
        return f ? Get(*f, rate, index) : nullptr;
    }
    void Put(const std::string& file, WaveTile tile)
    {
        FileTiles&    f = m_[file];
        const auto    k = std::make_pair(tile.rate, tile.index);
        if (f.find(k) == f.end()) ++n_;
        WaveTileSlot& s = f[k];
        s.tile = std::move(tile);
        s.used = ++tick_;
        ++gen_;
        while (n_ > cap_) {
            auto                of = m_.end();
            FileTiles::iterator oldest;
            for (auto fi = m_.begin(); fi != m_.end(); ++fi)
                for (auto it = fi->second.begin(); it != fi->second.end(); ++it)
                    if (of == m_.end() || it->second.used < oldest->second.used) {
                        of = fi;
                        oldest = it;
                    }
            if (of == m_.end()) break;
            of->second.erase(oldest);
            --n_;
            if (of->second.empty()) m_.erase(of);
        }
    }
    void EraseFile(const std::string& file)
    {
        auto f = m_.find(file);
        if (f == m_.end()) return;
        n_ -= f->second.size();
        m_.erase(f);
        ++gen_;
    }
    size_t   size() const { return n_; }
    void     clear()
    {
        m_.clear();
        n_ = 0;
        ++gen_;
    }
    uint64_t generation() const { return gen_; }

private:
    std::map<std::string, FileTiles> m_;
    size_t                           cap_;
    size_t                           n_ = 0;
    uint64_t                         tick_ = 0;
    uint64_t                         gen_ = 0;
};

// 13-57: which running read the UI thread gives up so a waiting file is read at once when every reader
// is busy. A read is given up when nobody asked for it these kPeakFreshSeconds while a waiting file was
// asked within them; 13-57 review: a detail read first (its tiles are asked again on the next frame,
// nothing is lost), then a read whose peaks are already shown (the .reapeaks build after a long file's
// first pass), else the one asked longest ago. -1 = none.
struct PeakRunning {
    double asked_s = 0.0;       // when its file (or a tile of it) was last asked
    bool   delivered = false;   // its peaks are shown: only the peak file's build goes on
    bool   tiles = false;       // a detail read (always given up first)
};

inline int PickPeakCancel(const std::vector<PeakRunning>& running, bool readers_full, bool fresh_waiting, double now_s)
{
    if (!readers_full || !fresh_waiting) return -1;
    auto rank = [](const PeakRunning& r) { return r.tiles ? 0 : r.delivered ? 1 : 2; };
    int  best = -1;
    for (size_t i = 0; i < running.size(); ++i) {
        const PeakRunning& r = running[i];
        if (!r.tiles && !r.delivered && !PeakAbandonInFlight(r.asked_s, true, now_s)) continue;  // still wanted (13-55 review)
        if (best < 0) {
            best = static_cast<int>(i);
            continue;
        }
        const PeakRunning& b = running[static_cast<size_t>(best)];
        if (rank(r) != rank(b) ? rank(r) < rank(b) : r.asked_s < b.asked_s) best = static_cast<int>(i);
    }
    return best;
}

// 13-57 review: the detail tiles one reader takes for a file, at most `max`: the ones asked last first
// (the current frame's zoom level before the levels a zoom passed through), the finer level first
// among those asked together, then left to right. Returns indices into `w`.
struct WaveTileWant {
    double  rate = 0.0;
    int64_t index = 0;
    double  asked_s = 0.0;
};

inline std::vector<size_t> PickTilesForJob(const std::vector<WaveTileWant>& w, size_t max)
{
    std::vector<size_t> k(w.size());
    for (size_t i = 0; i < k.size(); ++i) k[i] = i;
    std::sort(k.begin(), k.end(), [&w](size_t a, size_t b) {
        const WaveTileWant& x = w[a];
        const WaveTileWant& y = w[b];
        if (x.asked_s != y.asked_s) return x.asked_s > y.asked_s;
        if (x.rate != y.rate) return x.rate > y.rate;
        return x.index < y.index;
    });
    if (k.size() > max) k.resize(max);
    return k;
}

}  // namespace rav
