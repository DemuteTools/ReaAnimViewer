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
        CHECK(t.series == kWaveSeries && t.lo[3].size() == static_cast<size_t>(kWaveTileColumns));
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
        FoldWaveColumns(look, {1024.0}, sr, kWaveSeries, 0.0, 0.1, 100, coarse, &out);
        CHECK(out.series == kWaveSeries && out.detail);
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
        CHECK(w.series == kWaveSeries && !w.detail);
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
    }

    if (g_fail) std::printf("%d check(s) failed\n", g_fail);
    else std::printf("wave_tiles_plan: all checks passed\n");
    return g_fail ? 1 : 0;
}
