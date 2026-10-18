// SPDX-License-Identifier: MIT
//
// Host test of 13-57's detail waveform rules: src/wave_tiles_plan.h (the tile rate of a view, the
// tiles it needs, samples folded into tiles, tiles folded into one column per pixel with the levels
// around as a fallback, the columns joined, the sample line at extreme zoom, the LRU, the coarse
// peaks as symmetric columns, which running read is given up for a waiting file).

#include "wave_tiles_plan.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

static bool Near(double a, double b, double e = 1e-6) { return std::fabs(a - b) <= e; }

// A mono file of `n` samples at `sr`, sample k = f(k), read into the tiles of `rate`.
template <typename F>
static std::map<int64_t, WaveTile> ReadTiles(double rate, double sr, int64_t n, int nch, F f)
{
    std::map<int64_t, WaveTile> tiles;
    for (int64_t k = 0; k < n; ++k) {
        const int64_t ti = static_cast<int64_t>(std::floor(static_cast<double>(k) * rate / sr)) / kWaveTileColumns;
        auto          it = tiles.find(ti);
        if (it == tiles.end()) {
            WaveTile t;
            WaveTileInit(&t, rate, ti, nch);
            it = tiles.emplace(ti, std::move(t)).first;
        }
        std::vector<float> frame(static_cast<size_t>(nch));
        for (int c = 0; c < nch; ++c) frame[static_cast<size_t>(c)] = f(k, c);
        WaveTileAddFrame(&it->second, k, sr, frame.data(), nch);
    }
    return tiles;
}

int main()
{
    // ---- WaveTileRate: a power of two at or above the pixel rate, capped at the sample rate ----
    CHECK(WaveTileRate(1.0, 1000, 48000.0) == 1024.0);
    CHECK(WaveTileRate(0.5, 1000, 48000.0) == 2048.0);
    CHECK(WaveTileRate(1.0, 1024, 48000.0) == 1024.0);
    CHECK(WaveTileRate(0.01, 1000, 48000.0) == 48000.0);   // 100000 px/s: one column per sample
    CHECK(WaveTileRate(0.03, 1000, 48000.0) == 48000.0);   // 33333 px/s -> 65536 capped
    CHECK(WaveTileRate(kWaveDetailMaxSpan + 1.0, 1000, 48000.0) == 0.0);  // long view: the coarse peaks
    CHECK(WaveTileRate(kWaveDetailMaxSpan, 1000, 48000.0) == 32.0);
    CHECK(WaveTileRate(0.0, 1000, 48000.0) == 0.0 && WaveTileRate(1.0, 0, 48000.0) == 0.0);
    CHECK(WaveTileRate(1.0, 1000, 0.0) == 0.0);

    // ---- WaveTilesFor: the tiles a view needs, clamped to the file ----
    {
        int64_t a = -1, b = -1;
        CHECK(WaveTilesFor(0.0, 1.0, 1024.0, 3.0, &a, &b) && a == 0 && b == 2);
        CHECK(WaveTilesFor(0.6, 0.2, 1024.0, 3.0, &a, &b) && a == 1 && b == 1);
        CHECK(WaveTilesFor(2.9, 5.0, 1024.0, 3.0, &a, &b) && a == 5 && b == 5);  // past the end: the last tile
        CHECK(WaveTilesFor(-1.0, 1.4, 1024.0, 3.0, &a, &b) && a == 0 && b == 0);
        CHECK(!WaveTilesFor(5.0, 1.0, 1024.0, 3.0, &a, &b));  // all past the end
        CHECK(Near(WaveTileStart(1024.0, 2), 1.0) && Near(WaveTileEnd(1024.0, 2), 1.5));
    }

    // ---- WaveTileAddFrame: signed min / max per column, the series of a stereo file ----
    {
        WaveTile t;
        WaveTileInit(&t, 1000.0, 0, 2);
        CHECK(t.series == WaveSeriesCount(2) && t.series == 4 && t.lo[3].size() == static_cast<size_t>(kWaveTileColumns));
        const float f1[2] = {0.5f, -0.25f}, f2[2] = {-0.75f, 0.25f};
        WaveTileAddFrame(&t, 0, 48000.0, f1, 2);
        WaveTileAddFrame(&t, 47, 48000.0, f2, 2);  // still column 0 (47 * 1000 / 48000 < 1)
        CHECK(Near(t.lo[kWaveAll][0], -0.75) && Near(t.hi[kWaveAll][0], 0.5));
        CHECK(Near(t.lo[kWaveLeft][0], -0.75) && Near(t.hi[kWaveLeft][0], 0.5));
        CHECK(Near(t.lo[kWaveRight][0], -0.25) && Near(t.hi[kWaveRight][0], 0.25));
        CHECK(Near(t.lo[kWaveSum][0], -0.25) && Near(t.hi[kWaveSum][0], 0.125));
        CHECK(!(t.lo[0][1] <= t.hi[0][1]));  // nothing in column 1
        WaveTileAddFrame(&t, 48, 48000.0, f1, 2);  // column 1
        CHECK(Near(t.hi[0][1], 0.5));
        WaveTileAddFrame(&t, 48000 * 600, 48000.0, f1, 2);  // another tile: ignored
        // Sample rate level: one column per sample, exact.
        WaveTile u;
        WaveTileInit(&u, 44100.0, 3, 1);
        CHECK(u.series == 1 && u.lo[1].empty());
        const float v = 0.3f;
        WaveTileAddFrame(&u, 3 * kWaveTileColumns + 7, 44100.0, &v, 1);
        CHECK(Near(u.hi[0][7], 0.3) && Near(u.lo[0][7], 0.3));
        const float nanv = std::nanf("");
        WaveTileAddFrame(&u, 3 * kWaveTileColumns + 8, 44100.0, &nanv, 1);
        CHECK(!(u.lo[0][8] <= u.hi[0][8]));  // a NaN sample is skipped
    }

    // ---- FoldWaveColumns: one column per pixel from the wanted rate ----
    const double sr = 48000.0;
    auto         saw = [](int64_t k, int) { return static_cast<float>((k % 480) / 480.0 * 2.0 - 1.0); };  // 100 Hz saw
    {
        // A 1 s file, viewed whole over 1000 px: rate 1024; every pixel ~1 ms.
        auto tiles = ReadTiles(1024.0, sr, 48000, 1, saw);
        auto look = [&](double rate, int64_t i) -> const WaveTile* {
            if (rate != 1024.0) return nullptr;
            auto it = tiles.find(i);
            return it == tiles.end() ? nullptr : &it->second;
        };
        WaveColumns coarse, out;
        FoldWaveColumns(look, {1024.0}, sr, 1, 0.0, 1.0, 1000, coarse, &out);
        CHECK(out.detail && !out.line && out.columns == 1000 && out.lo[0].size() == 1000);
        for (int c = 0; c < 1000; ++c) CHECK(WaveColumnHas(out, 0, static_cast<size_t>(c)));
        // Pixel 0 (0..1 ms = samples 0..47 + the column at 1/1024 s): rising from -1.
        CHECK(Near(out.lo[0][0], -1.0, 1e-6) && out.hi[0][0] < -0.7f);
        // Pixel 9 (9..10 ms) holds the saw's wrap at 10 ms? No: the wrap is at sample 480 = 10 ms,
        // pixel 10's start: pixel 9 stays rising, pixel 10 starts again near -1.
        CHECK(out.hi[0][9] > 0.85f && out.lo[0][9] > 0.7f);
        CHECK(out.lo[0][10] < -0.95f);
        // Joined: pixel 10 now reaches up to pixel 9's low, so no gap is drawn on the wrap.
        JoinWaveColumns(&out);
        CHECK(out.hi[0][10] >= out.lo[0][9] - 1e-6f);
        CHECK(out.lo[0][9] <= out.hi[0][8] + 1e-6f);
    }
    {
        // Missing tiles: the pixel keeps the coarse column; another level fills what it holds.
        auto t1024 = ReadTiles(1024.0, sr, 48000, 1, saw);
        auto t512 = ReadTiles(512.0, sr, 48000, 1, saw);
        auto look = [&](double rate, int64_t i) -> const WaveTile* {
            if (rate == 1024.0 && i == 0) return &t1024.at(0);  // only the first half second at 1024
            if (rate == 512.0) {
                auto it = t512.find(i);
                return it == t512.end() ? nullptr : &it->second;
            }
            return nullptr;
        };
        WaveColumns coarse;
        WaveColumnsInit(&coarse, 1000, 1);
        for (size_t i = 0; i < 1000; ++i) {
            coarse.lo[0][i] = -2.0f;
            coarse.hi[0][i] = 2.0f;
        }
        WaveColumns out;
        FoldWaveColumns(look, WaveFallbackRates(1024.0, sr), sr, 1, 0.0, 1.0, 1000, coarse, &out);
        CHECK(!out.detail);  // part of the view came from another level
        CHECK(out.hi[0][100] < 1.5f && out.hi[0][900] < 1.5f);  // tiles everywhere: no coarse column left
        // Nothing at all: the coarse columns as given.
        auto none = [](double, int64_t) -> const WaveTile* { return nullptr; };
        FoldWaveColumns(none, {1024.0}, sr, 1, 0.0, 1.0, 1000, coarse, &out);
        CHECK(!out.detail && Near(out.hi[0][500], 2.0));
    }
    {
        // Extreme zoom: 10 ms over 1000 px = 100 px a sample at 48 kHz... the samples as a line.
        auto tiles = ReadTiles(sr, sr, 4800, 1, saw);
        auto look = [&](double rate, int64_t i) -> const WaveTile* {
            if (rate != sr) return nullptr;
            auto it = tiles.find(i);
            return it == tiles.end() ? nullptr : &it->second;
        };
        WaveColumns coarse, out;
        const double vs = 0.05, vl = 0.0005;  // 24 samples over 1000 px
        CHECK(WaveTileRate(vl, 1000, sr) == sr);
        FoldWaveColumns(look, WaveFallbackRates(sr, sr), sr, 1, vs, vl, 1000, coarse, &out);
        CHECK(out.detail && out.line);
        CHECK(out.pt_x.size() >= 24 && out.pt_x.size() <= 28 && out.pt_v[0].size() == out.pt_x.size());
        CHECK(out.pt_x.front() <= 0.0 && out.pt_x.back() >= 1000.0);  // the line reaches both edges
        // Sample 2400 (= 0.05 s) sits at x 0 with its value.
        bool found = false;
        for (size_t i = 0; i < out.pt_x.size(); ++i)
            if (Near(out.pt_x[i], 0.0, 1e-6)) found = Near(out.pt_v[0][i], saw(2400, 0), 1e-6);
        CHECK(found);
        // At 2 samples a pixel: columns, not a line.
        FoldWaveColumns(look, WaveFallbackRates(sr, sr), sr, 1, 0.0, 0.05, 1200, coarse, &out);
        CHECK(out.detail && !out.line);
    }
    {
        // A stereo file: the four series folded.
        auto tiles = ReadTiles(1024.0, sr, 4800, 2, [](int64_t, int c) { return c == 0 ? 0.5f : -0.5f; });
        auto look = [&](double rate, int64_t i) -> const WaveTile* {
            auto it = tiles.find(i);
            return rate == 1024.0 && it != tiles.end() ? &it->second : nullptr;
        };
        WaveColumns coarse, out;
        FoldWaveColumns(look, {1024.0}, sr, WaveSeriesCount(2), 0.0, 0.1, 100, coarse, &out);
        CHECK(out.series == 4 && out.detail);
        CHECK(Near(out.hi[kWaveLeft][5], 0.5) && Near(out.lo[kWaveRight][5], -0.5));
        CHECK(Near(out.hi[kWaveSum][5], 0.0) && Near(out.lo[kWaveAll][5], -0.5) && Near(out.hi[kWaveAll][5], 0.5));
    }

    // ---- WaveFallbackRates ----
    {
        const std::vector<double> r = WaveFallbackRates(1024.0, sr);
        CHECK(r.size() == 6 && r[0] == 1024.0 && r[1] == 2048.0 && r[2] == 512.0);
        const std::vector<double> s = WaveFallbackRates(sr, sr);
        CHECK(s[0] == sr && std::find(s.begin(), s.end(), 32768.0) != s.end());
        for (double v : s) CHECK(v <= sr);
        CHECK(WaveFallbackRates(0.0, sr).empty());
    }

    // ---- JoinWaveColumns: an empty column breaks the join ----
    {
        WaveColumns w;
        WaveColumnsInit(&w, 3, 1);
        w.lo[0] = {0.5f, std::numeric_limits<float>::max(), -0.9f};
        w.hi[0] = {0.6f, -std::numeric_limits<float>::max(), -0.8f};
        JoinWaveColumns(&w);
        CHECK(!WaveColumnHas(w, 0, 1) && Near(w.hi[0][2], -0.8));  // not joined across the gap
        w.lo[0] = {0.5f, -0.9f, 0.1f};
        w.hi[0] = {0.6f, -0.8f, 0.2f};
        JoinWaveColumns(&w);
        CHECK(Near(w.hi[0][1], 0.5) && Near(w.lo[0][1], -0.9));  // up to the column before
        CHECK(Near(w.lo[0][2], -0.8) && Near(w.hi[0][2], 0.2));   // down to the original one before
    }

    // ---- ColumnsFromPeaks: symmetric columns, no blocky stretch past the bins ----
    {
        PeakData p;
        p.rate = 1000.0;
        PeakInit(&p, 2, 1001);
        for (size_t i = 0; i < 1001; ++i) {
            p.max_abs[i] = 0.5f;
            p.lanes[0][i] = 0.5f;
            p.lanes[1][i] = 0.25f;
        }
        p.max_abs[500] = 0.9f;
        p.lanes[0][500] = 0.9f;
        WaveColumns w;
        ColumnsFromPeaks(p, 0.0, 1.0, 100, &w);
        CHECK(w.series == 4 && !w.detail);
        CHECK(Near(w.hi[0][50], 0.9) && Near(w.lo[0][50], -0.9) && Near(w.hi[0][49], 0.5));
        CHECK(Near(w.hi[kWaveRight][10], 0.25) && Near(w.hi[kWaveSum][10], 0.375));
        PeakData mono;
        mono.rate = 1000.0;
        PeakInit(&mono, 1, 11);
        ColumnsFromPeaks(mono, 0.0, 0.01, 4, &w);
        CHECK(w.series == 1 && WaveColumnHas(w, 0, 3));
        ColumnsFromPeaks(mono, 1.0, 0.01, 4, &w);  // past the peaks: nothing
        CHECK(!WaveColumnHas(w, 0, 0));
    }

    // ---- WaveTileLru ----
    {
        WaveTileLru lru(3);
        for (int i = 0; i < 3; ++i) {
            WaveTile t;
            WaveTileInit(&t, 1024.0, i, 1);
            lru.Put("a", std::move(t));
        }
        CHECK(lru.Get("a", 1024.0, 0) != nullptr);  // touched: 1 is now the oldest
        WaveTile t;
        WaveTileInit(&t, 1024.0, 9, 1);
        lru.Put("b", std::move(t));
        CHECK(lru.size() == 3 && lru.Get("a", 1024.0, 1) == nullptr && lru.Get("a", 1024.0, 0) && lru.Get("b", 1024.0, 9));
        CHECK(lru.Get("a", 512.0, 0) == nullptr);  // another rate: another tile
        lru.EraseFile("a");
        CHECK(lru.size() == 1 && lru.Get("b", 1024.0, 9));
    }

    // ---- PickPeakCancel ----
    {
        const double now = 100.0;
        std::vector<PeakRunning> r = {{99.9, false}, {99.0, false}, {98.0, false}};
        CHECK(PickPeakCancel(r, false, true, now) == -1);  // a reader is free
        CHECK(PickPeakCancel(r, true, false, now) == -1);  // nobody waits
        CHECK(PickPeakCancel(r, true, true, now) == 2);    // the one asked longest ago
        r[0].delivered = true;
        CHECK(PickPeakCancel(r, true, true, now) == 0);    // a build after its peaks are shown first
        std::vector<PeakRunning> busy = {{99.9, false}, {99.8, false}};
        CHECK(PickPeakCancel(busy, true, true, now) == -1);  // every read still wanted
        std::vector<PeakRunning> back = {{200.0, false}};
        CHECK(PickPeakCancel(back, true, true, now) == -1);  // a clock that went back keeps it
        CHECK(std::string(PeakReadPathName(PeakReadPath::SamplesThenBuild)).find("first") != std::string::npos);
        // 13-57 review: a detail read is given up first, even one asked just now (its tiles come back).
        std::vector<PeakRunning> mix = {{98.0, false, false}, {99.0, true, false}, {99.95, false, true}};
        CHECK(PickPeakCancel(mix, true, true, now) == 2);
        std::vector<PeakRunning> two = {{99.95, false, true}, {99.5, false, true}, {99.99, false, false}};
        CHECK(PickPeakCancel(two, true, true, now) == 1);  // of two detail reads, the one asked longer ago
        CHECK(PickPeakCancel(two, false, true, now) == -1);  // a reader is free: nothing given up
    }

    // ---- PickTilesForJob (13-57 review) ----
    {
        // A zoom passed through 256 and 512 (asked earlier) to 1024 (asked this frame).
        std::vector<WaveTileWant> w = {{256.0, 0, 10.0}, {256.0, 1, 10.0}, {512.0, 3, 10.2},
                                       {1024.0, 7, 10.4}, {1024.0, 6, 10.4}, {2048.0, 1, 10.4}};
        const std::vector<size_t> k = PickTilesForJob(w, 4);
        CHECK(k.size() == 4);
        CHECK(k[0] == 5);                // asked now, the finer level first
        CHECK(k[1] == 4 && k[2] == 3);   // then the current level left to right
        CHECK(k[3] == 2);                // then the level asked just before
        CHECK(PickTilesForJob(w, 16).size() == w.size() && PickTilesForJob({}, 4).empty());
    }

    // ---- FoldWaveColumns at the file's edges (13-57 review) ----
    {
        // A 0.1 s file at 48 kHz viewed at extreme zoom over its end: the pixels past the end need no
        // tile, so the read is complete and the samples still show as a line.
        auto tiles = ReadTiles(sr, sr, 4800, 1, saw);
        auto look = [&](double rate, int64_t i) -> const WaveTile* {
            if (rate != sr) return nullptr;
            auto it = tiles.find(i);
            return it == tiles.end() ? nullptr : &it->second;
        };
        WaveColumns  coarse, out;
        const double vs = 0.0998, vl = 0.01;  // 0.2 ms of file then 9.8 ms past its end (past its last tile)
        FoldWaveColumns(look, WaveFallbackRates(sr, sr), sr, 1, vs, vl, 1000, coarse, &out, 0.1);
        CHECK(out.detail && out.line);
        CHECK(!WaveColumnHas(out, 0, 999));  // past the end: nothing drawn
        // Without the length the pixels past the end wait for a tile that never comes (the old rule).
        FoldWaveColumns(look, WaveFallbackRates(sr, sr), sr, 1, vs, vl, 1000, coarse, &out);
        CHECK(!out.detail && !out.line);
        // Before 0 s: the view starts left of the file.
        FoldWaveColumns(look, WaveFallbackRates(sr, sr), sr, 1, -0.0003, 0.0005, 1000, coarse, &out, 0.1);
        CHECK(out.detail && out.line && !WaveColumnHas(out, 0, 0));
    }

    // ---- WaveTileLru generation (13-57 review) ----
    {
        WaveTileLru lru(2);
        const uint64_t g0 = lru.generation();
        CHECK(lru.Get("a", 1.0, 0) == nullptr && lru.generation() == g0);  // a lookup changes nothing
        WaveTile t;
        WaveTileInit(&t, 1024.0, 0, 1);
        lru.Put("a", t);
        CHECK(lru.generation() > g0);
        const uint64_t g1 = lru.generation();
        lru.EraseFile("zzz");
        CHECK(lru.generation() == g1);  // nothing erased
        lru.EraseFile("a");
        CHECK(lru.generation() > g1 && lru.size() == 0 && lru.Of("a") == nullptr);
    }

    // ---- 13-71: one lane per channel (WaveLanesFor), the series per channel ----
    {
        CHECK(WaveSeriesCount(0) == 1 && WaveSeriesCount(1) == 1 && WaveSeriesCount(2) == 4 && WaveSeriesCount(6) == 8);
        CHECK(WaveSeriesCount(8) == kWaveSeries && WaveSeriesCount(32) == kWaveSeries);  // capped
        CHECK(WaveSeriesChannels(1) == 1 && WaveSeriesChannels(4) == 2 && WaveSeriesChannels(8) == 6);
        CHECK(WaveChannelSeries(0) == kWaveLeft && WaveChannelSeries(1) == kWaveRight && WaveChannelSeries(2) == 4);
        CHECK(WaveChannelSeries(7) == 9 && WaveChannelSeries(8) == kWaveLeft);  // past the cap: folds
        // Mono file: one lane, series 0, whatever the Sound's mono mode.
        for (MonoMode m : {MonoMode::Off, MonoMode::Left, MonoMode::Right, MonoMode::Sum}) {
            const WaveLanes w = WaveLanesFor(m, 1);
            CHECK(w.n == 1 && w.series[0] == kWaveAll);
        }
        // Stereo: two lanes L above R; forced mono: one lane, the channel (or the sum) heard.
        WaveLanes w = WaveLanesFor(MonoMode::Off, WaveSeriesCount(2));
        CHECK(w.n == 2 && w.series[0] == kWaveLeft && w.series[1] == kWaveRight);
        w = WaveLanesFor(MonoMode::Left, 4);
        CHECK(w.n == 1 && w.series[0] == kWaveLeft);
        w = WaveLanesFor(MonoMode::Right, 4);
        CHECK(w.n == 1 && w.series[0] == kWaveRight);
        w = WaveLanesFor(MonoMode::Sum, 4);
        CHECK(w.n == 1 && w.series[0] == kWaveSum);
        // 6 channels: six lanes, in channel order; forced mono still one.
        w = WaveLanesFor(MonoMode::Off, WaveSeriesCount(6));
        CHECK(w.n == 6 && w.series[0] == kWaveLeft && w.series[1] == kWaveRight && w.series[2] == 4 && w.series[5] == 7);
        CHECK(WaveLanesFor(MonoMode::Sum, WaveSeriesCount(6)).n == 1);
        CHECK(WaveLanesFor(MonoMode::Off, WaveSeriesCount(20)).n == kWaveMaxLanes);

        // A 6-channel tile: each channel in its own series, the sum of ch 1 and 2.
        WaveTile t;
        WaveTileInit(&t, 1000.0, 0, 6);
        CHECK(t.series == 8 && t.lo[7].size() == static_cast<size_t>(kWaveTileColumns) && t.lo[8].empty());
        const float f[6] = {0.1f, -0.2f, 0.7f, 0.3f, -0.4f, 0.6f};  // L R C LFE Ls Rs
        WaveTileAddFrame(&t, 0, 48000.0, f, 6);
        CHECK(Near(t.hi[kWaveAll][0], 0.7) && Near(t.lo[kWaveAll][0], -0.4));
        CHECK(Near(t.hi[kWaveLeft][0], 0.1) && Near(t.lo[kWaveRight][0], -0.2));
        CHECK(Near(t.hi[WaveChannelSeries(2)][0], 0.7) && Near(t.hi[WaveChannelSeries(3)][0], 0.3));
        CHECK(Near(t.lo[WaveChannelSeries(4)][0], -0.4) && Near(t.hi[WaveChannelSeries(5)][0], 0.6));
        CHECK(Near(t.hi[kWaveSum][0], -0.05));

        // The coarse peaks of 3 channels: three lanes.
        PeakData p;
        PeakInit(&p, 3, 4);
        p.rate = 4.0;
        const float fr[3] = {0.5f, 0.25f, 0.75f};
        for (size_t i = 0; i < 4; ++i) PeakAddFrame(&p, i, fr, 3);
        WaveColumns c;
        ColumnsFromPeaks(p, 0.0, 1.0, 4, &c);
        CHECK(c.series == 5 && WaveLanesFor(MonoMode::Off, c.series).n == 3);
        CHECK(Near(c.hi[kWaveLeft][1], 0.5) && Near(c.hi[kWaveRight][1], 0.25) && Near(c.hi[WaveChannelSeries(2)][1], 0.75));
        CHECK(Near(c.lo[WaveChannelSeries(2)][1], -0.75) && Near(c.hi[kWaveSum][1], 0.375) && Near(c.hi[kWaveAll][1], 0.75));

        // 13-71 review: lane geometry (the waveform and Sound Edit's threshold line read the same).
        const WaveLaneGeom m0 = WaveLaneAt(0.0f, 100.0f, 1, 6.0f, 0);  // one lane: the row's middle, half less the pad
        CHECK(Near(m0.mid, 50.0) && Near(m0.hh, 44.0));
        const WaveLaneGeom s0 = WaveLaneAt(0.0f, 100.0f, 2, 6.0f, 0), s1 = WaveLaneAt(0.0f, 100.0f, 2, 6.0f, 1);
        CHECK(Near(s0.mid, 25.0) && Near(s1.mid, 75.0) && Near(s0.hh, 19.0) && Near(s1.hh, 19.0));
        const WaveLaneGeom e7 = WaveLaneAt(0.0f, 40.0f, 8, 6.0f, 7);  // a 5 px lane: the pad at most a fifth of it
        CHECK(Near(e7.mid, 37.5) && Near(e7.hh, 1.5));
        CHECK(Near(WaveLaneAt(0.0f, 4.0f, 8, 6.0f, 0).hh, 1.0) && Near(WaveLaneAt(0.0f, 10.0f, 0, 2.0f, 0).mid, 5.0));
    }

    // ---- PeakBlockFrames (13-57 review) ----
    {
        CHECK(PeakBlockFrames(1) == 65536 && PeakBlockFrames(2) == 65536 && PeakBlockFrames(8) == 65536);
        CHECK(PeakBlockFrames(16) == 32768 && PeakBlockFrames(64) == 8192);
        CHECK(PeakBlockFrames(100000) == 256 && PeakBlockFrames(0) == 65536);
    }

    if (g_fail) std::printf("%d check(s) failed\n", g_fail);
    else std::printf("wave_tiles_plan: all checks passed\n");
    return g_fail ? 1 : 0;
}
