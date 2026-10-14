// SPDX-License-Identifier: MIT
//
// See sound_peaks.h. 13-55: the one peak reader of the Sound panel, Edit and the Media Explorer
// (13-7c's own reader folded in). Threads: the UI thread opens a file (PCM_Source_CreateFromFile)
// and deletes its source; a reader thread, given that source alone, makes sure REAPER's .reapeaks
// file is there (PeaksBuild_Begin / Run / Finish: read when it is there, built and written as REAPER
// does when not) and reads the peaks (GetPeakInfo for a long file, GetSamples for a short one,
// sound_peaks_plan.h). The UI thread polls the results in RequestPeaks (a map lookup and a locked
// swap: no REAPER call per frame) and hands the next waiting file over.
// 13-57: kPeakReaders readers side by side (a clicked file never waits behind another file's read);
// a long file with no peak file is shown from a first read of its samples, its peak file built after;
// and a waveform's view is read at screen resolution from the samples in tiles (ReadWaveColumns,
// wave_tiles_plan.h), kept in a small LRU.

#include "sound_peaks.h"

#ifdef _WIN32

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "console_log.h"
#include "file_change_check.h"
#include "reaper_item_read.h"
#include "sound_peaks_plan.h"
#include "sound_populate.h"
#include "wave_tiles_plan.h"

namespace rav {
namespace {

using CreateFromFileFn = PCM_source* (*)(const char*);
using GetNumTakeMarkersFn = int (*)(MediaItem_Take*);
using GetTakeMarkerFn = double (*)(MediaItem_Take*, int, char*, int, int*);
using GetItemStateChunkFn = bool (*)(MediaItem*, char*, int, bool);
using CountTakesFn = int (*)(MediaItem*);
using GetMediaItemTakeFn = MediaItem_Take* (*)(MediaItem*, int);
using TakeFXGetCountFn = int (*)(MediaItem_Take*);
using GetPeakFileNameExFn = void (*)(const char*, char*, int, bool);

CreateFromFileFn    f_create = nullptr;
GetNumTakeMarkersFn f_num_tm = nullptr;
GetTakeMarkerFn     f_get_tm = nullptr;
GetItemStateChunkFn f_chunk = nullptr;
CountTakesFn        f_count_takes = nullptr;
GetMediaItemTakeFn  f_get_take = nullptr;
TakeFXGetCountFn    f_takefx_count = nullptr;  // 13-49 review: optional (the "Placed without" line)
GetPeakFileNameExFn f_peak_name = nullptr;     // 13-57 review: optional (a long file's second source)

constexpr size_t kCacheFiles = 32;
// Frames per GetSamples call: PeakBlockFrames (13-57: was 8192; fewer calls a read, capped by channels).
constexpr int    kPeakChunk = 4096;  // points per GetPeakInfo call (a long file)
// 13-57: reads run side by side (each on a source of its own): the file just clicked starts at once
// instead of waiting behind another file's read or a long file's peak build. The detail tiles use at
// most kPeakReaders - 1 of them: one reader is always there for a file's peaks.
constexpr size_t kPeakReaders = 3;
constexpr size_t kTilesPerJob = 16;
// 13-57 review: a file's detail reads reuse one source kept between them (no open on the UI thread per
// tile read), for at most this many files.
constexpr size_t kIdleSources = 4;
constexpr size_t kWaveMemos = 8;  // the last waveforms built (one per waveform on screen)

struct Entry {
    std::string path;  // as first asked: the file is opened with it
    PeakState   state = PeakState::Building;
    PeakData    peaks;
    double      length = 0.0;
    double      sr = 0.0;           // 13-57: its sample rate and channels (the detail tiles read them)
    int         nch = 0;
    uint64_t    used = 0;
    uint64_t    seq = 0;            // the order it was asked in: its read's id
    double      asked_s = 0.0;      // Building: when it was last asked (NowSeconds)
    double      first_asked_s = 0.0;  // 13-57 (debuglog): when it was first asked
    bool        in_flight = false;  // Building: a reader has it
    FileVersion version;            // 13-51: the file's date + size before it was read
    double      checked_s = 0.0;    // when the disk was last asked (NowSeconds)
};

enum class JobKind { Peaks, Tiles };

// A reader's work: the source(s) the UI thread opened, handed back (the UI thread deletes them).
struct Job {
    uint64_t    id = 0;
    JobKind     kind = JobKind::Peaks;
    std::string key;
    uint64_t    seq = 0;
    PCM_source* src = nullptr;
    PCM_source* src2 = nullptr;  // 13-57: a long file's second source (its first pass while src builds)
    double      length = 0.0, sr = 0.0;
    int         nch = 0;
    std::vector<std::pair<double, int64_t>> tiles;  // Tiles: (rate, index)
    FileVersion version;
    std::shared_ptr<std::atomic<bool>> cancel;  // set by the UI thread: the read is given up
    std::chrono::steady_clock::time_point pushed;
    double      open_ms = 0.0;  // debuglog: PCM_Source_CreateFromFile's time on the UI thread
};
struct JobResult {
    uint64_t    id = 0;
    JobKind     kind = JobKind::Peaks;
    std::string key;
    uint64_t    seq = 0;
    bool        final = true;   // 13-57: false = a long file's first pass (its peak file is still built)
    PCM_source* src = nullptr;  // final only
    PCM_source* src2 = nullptr;
    bool        ok = false;
    bool        abandoned = false;  // given up for another file (it waits again)
    PeakData    peaks;
    PeakReadPath how = PeakReadPath::Samples;
    std::vector<WaveTile> tiles;
    FileVersion version;            // Tiles: the file's version its source was opened at
    double      wait_ms = 0.0, ms = 0.0, open_ms = 0.0;  // debuglog builds say them
};

// The UI thread's record of a read out on a reader.
struct Running {
    JobKind     kind = JobKind::Peaks;
    std::string key;
    uint64_t    seq = 0;
    bool        delivered = false;  // a long file's first pass is shown: only its peak file's build goes on
    std::vector<std::pair<double, int64_t>> tiles;
    std::shared_ptr<std::atomic<bool>> cancel;
};

// A detail tile asked by a waveform: when last asked, and whether a reader has it. By file, then
// (rate, index): a lookup copies no path (13-57 review).
using TileId = std::pair<double, int64_t>;
struct TileAsk {
    double asked_s = 0.0;
    bool   in_flight = false;
};
using FileAsks = std::map<TileId, TileAsk>;

// 13-57 review: a file's source kept between its detail reads (opened at `version`).
struct IdleSource {
    PCM_source* src = nullptr;
    FileVersion version;
    uint64_t    used = 0;
};

// 13-57 review: a waveform built (ReadWaveColumns), kept while nothing it was built from changed.
struct WaveMemo {
    std::string key;
    uint64_t    seq = 0;  // the entry's read (its peaks and version)
    double      vs = 0.0, vl = 0.0;
    int         cols = 0;
    uint64_t    gen = 0;  // g_tiles.generation()
    uint64_t    used = 0;
    WaveColumns wc;
};

std::atomic<bool> g_quit{false};

// Never destroyed: a building entry's REAPER source is not deleted from static destructors at
// unload (the process is going away; REAPER may already be gone).
std::map<std::string, Entry>& g_cache = *new std::map<std::string, Entry>();  // by PeakCacheKey
uint64_t                      g_tick = 0;
uint64_t                      g_seq = 0;
uint64_t                      g_job_id = 0;
std::map<uint64_t, Running>&  g_running = *new std::map<uint64_t, Running>();  // by job id
std::map<std::string, FileAsks>& g_tile_ask = *new std::map<std::string, FileAsks>();  // by PeakCacheKey
WaveTileLru&                  g_tiles = *new WaveTileLru();  // by PeakCacheKey
std::map<std::string, IdleSource>& g_idle = *new std::map<std::string, IdleSource>();  // by PeakCacheKey
std::vector<WaveMemo>&        g_wave_memo = *new std::vector<WaveMemo>();
uint64_t                      g_memo_tick = 0;
std::map<std::string, double> g_len;  // by PeakCacheKey (13-55 review), as g_cache
uint64_t                      g_len_gen = 0;  // bumped when a g_len entry is dropped

double NowSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double MsSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// The file's date and size (zero when it cannot be read).
FileVersion VersionOf(const std::string& path)
{
    FileVersion v;
    try {
        std::error_code             ec;
        const std::filesystem::path p = std::filesystem::u8path(path);
        const auto                  t = std::filesystem::last_write_time(p, ec);
        if (!ec) v.time = static_cast<long long>(t.time_since_epoch().count());
        const auto sz = std::filesystem::file_size(p, ec);
        if (!ec) v.size = static_cast<unsigned long long>(sz);
    } catch (...) {
        // a malformed path: unreadable
    }
    return v;
}

// 13-57 review: a peaks read of `key` is out on a reader (a long file's .reapeaks build goes on after
// its first pass is shown): no second read of that file starts meanwhile (two builds of one peak file).
bool PeaksRunning(const std::string& key)
{
    for (const auto& kv : g_running)
        if (kv.second.kind == JobKind::Peaks && kv.second.key == key) return true;
    return false;
}

void DropIdleSource(const std::string& key)
{
    const auto it = g_idle.find(key);
    if (it == g_idle.end()) return;
    delete it->second.src;
    g_idle.erase(it);
}

// The kept source of `key` for a detail read (null when none, or the file changed since).
PCM_source* TakeIdleSource(const std::string& key, const FileVersion& version)
{
    const auto it = g_idle.find(key);
    if (it == g_idle.end()) return nullptr;
    PCM_source* s = it->second.src;
    const bool  same = it->second.version == version;
    g_idle.erase(it);
    if (same) return s;
    delete s;
    return nullptr;
}

// A detail read's source back from its reader: kept for the next one (the least used dropped past
// kIdleSources), or deleted when one is kept already.
void KeepIdleSource(const std::string& key, PCM_source* s, const FileVersion& version)
{
    if (!s) return;
    if (g_idle.count(key)) {
        delete s;
        return;
    }
    static uint64_t tick = 0;
    g_idle[key] = IdleSource{s, version, ++tick};
    while (g_idle.size() > kIdleSources) {
        auto oldest = g_idle.begin();
        for (auto it = g_idle.begin(); it != g_idle.end(); ++it)
            if (it->second.used < oldest->second.used) oldest = it;
        delete oldest->second.src;
        g_idle.erase(oldest);
    }
}

void Evict()
{
    while (g_cache.size() > kCacheFiles) {
        auto oldest = g_cache.end();
        for (auto it = g_cache.begin(); it != g_cache.end(); ++it)
            if (it->second.state != PeakState::Building && !PeaksRunning(it->first) &&
                (oldest == g_cache.end() || it->second.used < oldest->second.used))
                oldest = it;
        if (oldest == g_cache.end()) return;
        DropIdleSource(oldest->first);
        g_cache.erase(oldest);
    }
}

// A file's detail is dropped (it changed on disk): its tiles, the tiles waiting for a reader, its kept
// source.
void ForgetTiles(const std::string& key)
{
    g_tiles.EraseFile(key);
    DropIdleSource(key);
    const auto f = g_tile_ask.find(key);
    if (f == g_tile_ask.end()) return;
    for (auto it = f->second.begin(); it != f->second.end();)
        it = !it->second.in_flight ? f->second.erase(it) : std::next(it);
    if (f->second.empty()) g_tile_ask.erase(f);
}

// ---- The readers (each only calls the sources it was handed) -------------------------------------

// The read in progress stops: unload, or given up for another file.
bool Stopped(const Job& j)
{
    return g_quit || (j.cancel && j.cancel->load());
}

// A long file: REAPER's peaks (from its .reapeaks file) at `rate`. False when REAPER gave no point
// at all (13-55 review: a silent file's zero peaks are its peaks, not a reason to read its samples).
bool ReadPeakInfo(PCM_source* s, const Job& j, double rate, PeakData* out)
{
    const size_t n = PeakBins(j.length, rate);
    out->rate = rate;
    PeakInit(out, j.nch, n);  // lanes: stereo only
    const size_t           per = static_cast<size_t>(kPeakChunk) * static_cast<size_t>(j.nch);
    std::vector<ReaSample> mx(per), mn(per);
    bool any = false;
    for (size_t i = 0; i < n && !Stopped(j);) {
        const int                 want = static_cast<int>(std::min<size_t>(kPeakChunk, n - i));
        PCM_source_peaktransfer_t pt = {};
        pt.start_time = static_cast<double>(i) / rate;
        pt.peakrate = rate;
        pt.numpeak_points = want;
        pt.nchpeaks = j.nch;
        pt.peaks = mx.data();
        pt.peaks_minvals = mn.data();
        pt.output_mode = PCM_source_peaktransfer_t::PEAKTRANSFER_PEAKS_MODE;
        pt.samplerate = j.sr;
        s->GetPeakInfo(&pt);
        const int got = std::max(0, std::min(pt.peaks_out, want));
        if (got == 0) break;  // the end (or no peaks at all)
        any = true;
        FoldPeakPoints(out, i, mx.data(), pt.peaks_minvals_used ? mn.data() : nullptr, got, j.nch);
        i += static_cast<size_t>(got);
    }
    return !Stopped(j) && any;
}

// The frames [n0, n1) of `s` in blocks of PeakBlockFrames, each handed to `each(n, frame)`. False when stopped.
template <typename Each>
bool ForEachFrame(PCM_source* s, const Job& j, int64_t n0, int64_t n1, std::vector<ReaSample>& buf, Each each)
{
    const int block = PeakBlockFrames(j.nch);
    buf.resize(static_cast<size_t>(block) * static_cast<size_t>(j.nch));
    for (int64_t done = n0; done < n1;) {
        if (Stopped(j)) return false;
        PCM_source_transfer_t tr = {};
        tr.time_s = static_cast<double>(done) / j.sr;
        tr.samplerate = j.sr;
        tr.nch = j.nch;
        tr.length = static_cast<int>(std::min<int64_t>(block, n1 - done));
        tr.samples = buf.data();
        s->GetSamples(&tr);
        const int got = tr.samples_out > 0 ? std::min(tr.samples_out, tr.length) : 0;
        for (int k = 0; k < got; ++k) each(done + k, buf.data() + static_cast<size_t>(k) * static_cast<size_t>(j.nch));
        done += tr.length;  // a short read is silence: never stall on it
    }
    return true;
}

// The file's samples folded at `rate` peaks/s (a short file at kPeakHiResRate: 13-7b's read).
bool ReadSamples(PCM_source* s, const Job& j, double rate, PeakData* out)
{
    const int64_t total = static_cast<int64_t>(std::ceil(j.length * j.sr));
    out->rate = rate;
    PeakInit(out, j.nch, PeakBins(j.length, rate));  // lanes: stereo only
    std::vector<ReaSample> buf;
    return ForEachFrame(s, j, 0, total, buf, [&](int64_t n, const ReaSample* f) {
        PeakAddFrame(out, static_cast<size_t>(static_cast<double>(n) * rate / j.sr), f, j.nch);
    });
}

// 13-57: a waveform's detail tiles, from the samples. The tiles read before a stop are kept.
void ReadTiles(const Job& j, std::vector<WaveTile>* out)
{
    const int64_t          total = static_cast<int64_t>(std::ceil(j.length * j.sr));
    std::vector<ReaSample> buf;
    for (const auto& rt : j.tiles) {
        WaveTile t;
        WaveTileInit(&t, rt.first, rt.second, j.nch);
        t.file = j.version;
        const int64_t n0 = std::max<int64_t>(0, static_cast<int64_t>(std::floor(WaveTileStart(rt.first, rt.second) * j.sr)));
        const int64_t n1 = std::min(total, static_cast<int64_t>(std::ceil(WaveTileEnd(rt.first, rt.second) * j.sr)) + 1);
        if (!ForEachFrame(j.src, j, n0, n1, buf, [&](int64_t n, const ReaSample* f) { WaveTileAddFrame(&t, n, j.sr, f, j.nch); }))
            return;
        out->push_back(std::move(t));
    }
}

using Post = std::function<void(JobResult&&)>;

// A short file (up to 10 s) is read from its samples alone: no peak file is built for it (13-55
// review). A long one: REAPER's own peak file (PeaksBuild_Begin answers 0 when it is there and up to
// date), its samples only when REAPER gave no peak point at all. 13-57: a long file with no peak file
// yet is first read from its samples on its second source (its waveform shows: `post` with final =
// false), then its peak file is built and written where REAPER keeps its peaks, as REAPER does for a
// file it plays (the next read of it is instant).
//
// A build stopped part way (unload, or given up for another file) is NOT finished: the SDK only says
// PeaksBuild_Finish is "called when done", not whether it would keep a part-built peak file as
// complete, so the source is deleted with its build open and nothing is deleted on disk (13-55
// review); the next read of that file builds it again.
bool ReadPeaksJob(const Job& j, JobResult* r, const Post& post)
{
    PCM_source* s = j.src;
    r->how = PeakReadPath::Samples;
    if (PeakReadsSamples(j.length)) return ReadSamples(s, j, kPeakHiResRate, &r->peaks);
    r->how = PeakReadPath::PeakFile;
    if (s->PeaksBuild_Begin()) {
        if (j.src2) {
            JobResult first;
            first.id = j.id;
            first.kind = j.kind;
            first.key = j.key;
            first.seq = j.seq;
            first.final = false;
            first.how = PeakReadPath::SamplesThenBuild;
            first.wait_ms = r->wait_ms;
            first.open_ms = j.open_ms;
            const auto t0 = std::chrono::steady_clock::now();
            first.ok = ReadSamples(j.src2, j, kPeakFileRate, &first.peaks);
            first.ms = MsSince(t0);
            if (!first.ok) return false;
            post(std::move(first));
            r->how = PeakReadPath::SamplesThenBuild;
        } else {
            r->how = PeakReadPath::BuiltThenPeakFile;
        }
        bool more = true;
        while (more && !Stopped(j)) more = s->PeaksBuild_Run() != 0;
        if (more) return false;  // stopped mid-build: not finished (above)
        s->PeaksBuild_Finish();
        if (j.src2) return true;  // its peaks are shown already
    }
    if (Stopped(j)) return false;
    if (ReadPeakInfo(s, j, ChoosePeakRate(j.length), &r->peaks)) return true;
    if (Stopped(j)) return false;
    r->how = PeakReadPath::SamplesFallback;
    r->peaks = PeakData{};
    return ReadSamples(s, j, kPeakFileRate, &r->peaks);
}

// kPeakReaders threads; the UI thread hands out at most one job per thread (never a queue behind one).
class PeakReader {
public:
    void Push(Job j)
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            q_.push_back(std::move(j));
            if (th_.size() < kPeakReaders) th_.emplace_back([this] { Run(); });
        }
        cv_.notify_one();
    }
    std::vector<JobResult> Take()
    {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<JobResult>      r;
        r.swap(out_);
        return r;
    }
    // Unload: the reads in progress stop (g_quit), the threads are joined, every source deleted.
    void Stop()
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        for (std::thread& t : th_)
            if (t.joinable()) t.join();
        th_.clear();
        for (JobResult& r : Take()) {
            delete r.src;
            delete r.src2;
        }
        std::lock_guard<std::mutex> lk(m_);
        for (Job& j : q_) {
            delete j.src;
            delete j.src2;
        }
        q_.clear();
        stop_ = false;
    }

private:
    void Run()
    {
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                if (stop_) return;
                j = std::move(q_.front());
                q_.pop_front();
            }
            JobResult r;
            r.id = j.id;
            r.kind = j.kind;
            r.key = j.key;
            r.seq = j.seq;
            r.src = j.src;
            r.src2 = j.src2;
            r.version = j.version;
            r.open_ms = j.open_ms;
            r.wait_ms = MsSince(j.pushed);
            const Post post = [this](JobResult&& x) {
                std::lock_guard<std::mutex> lk(m_);
                out_.push_back(std::move(x));
            };
            // Nothing may escape this thread (a throw would end REAPER): a failed read still answers.
            const auto t0 = std::chrono::steady_clock::now();
            try {
                if (j.kind == JobKind::Tiles) {
                    if (j.src) ReadTiles(j, &r.tiles);
                    r.ok = !r.tiles.empty();
                } else {
                    r.ok = j.src && ReadPeaksJob(j, &r, post);
                }
            } catch (...) {
                r.ok = false;
            }
            r.ms = MsSince(t0);
            r.abandoned = !g_quit && j.cancel && j.cancel->load();
            if (!r.ok) r.peaks = PeakData{};
            post(std::move(r));
        }
    }

    std::vector<std::thread> th_;
    std::mutex               m_;
    std::condition_variable  cv_;
    std::deque<Job>          q_;
    std::vector<JobResult>   out_;
    bool                     stop_ = false;
};

PeakReader& g_reader = *new PeakReader();  // never destroyed: joined by QuitSoundPeaks

// ---- The UI thread's side ------------------------------------------------------------------------

// The reads that came back: their entries are Ready (or Failed), their tiles kept; every source is
// deleted here.
void Collect()
{
    for (JobResult& r : g_reader.Take()) {
        std::vector<std::pair<double, int64_t>> asked_tiles;  // a detail read's tiles
        if (r.final) {
            const auto e = g_cache.find(r.key);
            if (r.kind == JobKind::Tiles && e != g_cache.end() && e->second.state == PeakState::Ready &&
                e->second.version == r.version)
                KeepIdleSource(r.key, r.src, r.version);  // 13-57 review: the next detail read reuses it
            else
                delete r.src;
            delete r.src2;
            auto run = g_running.find(r.id);
            if (run != g_running.end()) {
                asked_tiles = std::move(run->second.tiles);
                g_running.erase(run);
            }
        } else {
            auto run = g_running.find(r.id);
            if (run != g_running.end()) run->second.delivered = true;
        }
        if (r.kind == JobKind::Tiles) {
            // Its tiles leave the wait (the ones not read, given up, wait again when asked again).
            const auto fa = g_tile_ask.find(r.key);
            if (fa != g_tile_ask.end()) {
                for (const auto& t : asked_tiles) fa->second.erase(t);
                if (fa->second.empty()) g_tile_ask.erase(fa);
            }
            const auto it = g_cache.find(r.key);
            for (WaveTile& t : r.tiles)
                if (it != g_cache.end() && it->second.state == PeakState::Ready && it->second.version == t.file)
                    g_tiles.Put(r.key, std::move(t));
#ifdef RAV_ENABLE_CONSOLE_LOG
            LogInfo("peaks: %s: %d detail tile(s)%s; waited %.0f ms for a reader, opened in %.0f ms, read in %.0f ms",
                    r.key.c_str(), static_cast<int>(r.tiles.size()), r.abandoned ? " (given up)" : "", r.wait_ms,
                    r.open_ms, r.ms);
#endif
            continue;
        }
        auto it = g_cache.find(r.key);
        if (it == g_cache.end() || it->second.seq != r.seq) continue;  // dropped meanwhile
        Entry& e = it->second;
#ifdef RAV_ENABLE_CONSOLE_LOG
        // 13-55 review / 13-57 (debuglog builds only): which way this file's read went, and each stage's
        // time: the wait for a reader, the open (UI thread), the read, and since the file was first asked.
        if (r.ok)
            LogInfo("peaks: %s: %s%s; waited %.0f ms for a reader, opened in %.0f ms, %s %.0f ms, shown %.0f ms after first asked",
                    e.path.c_str(), PeakReadPathName(r.how), r.final ? "" : " (first pass)", r.wait_ms, r.open_ms,
                    r.final && r.how == PeakReadPath::SamplesThenBuild ? "peak file built in" : "read in", r.ms,
                    (NowSeconds() - e.first_asked_s) * 1000.0);
#endif
        if (e.state != PeakState::Building) continue;  // a long file's build after its first pass
        e.in_flight = false;
        if (r.abandoned && !r.ok) continue;  // 13-55 review: waits again (dropped if nobody asks for it)
        e.state = r.ok ? PeakState::Ready : PeakState::Failed;
        if (r.ok) e.peaks = std::move(r.peaks);
        e.used = ++g_tick;  // just read: not the first evicted before its asker takes it
    }
}

// The reads nobody waits for any more are given up: a detail read whose tiles nobody asked for these
// kPeakFreshSeconds (the view moved on); when every reader is busy and a waiting file was just asked,
// the read nobody asks for (PickPeakCancel: a detail read first, then a long file's peak build after its
// first pass).
void CancelUnwanted(double now)
{
    bool fresh_waiting = false;
    for (const auto& kv : g_cache)
        if (kv.second.state == PeakState::Building && !kv.second.in_flight && now >= kv.second.asked_s &&
            now - kv.second.asked_s <= kPeakFreshSeconds && !PeaksRunning(kv.first))
            fresh_waiting = true;
    std::vector<PeakRunning>    runs;
    std::vector<const Running*> who;
    bool                        stopping = false;  // a read given up has not come back yet
    for (const auto& kv : g_running) {
        const Running& r = kv.second;
        if (r.cancel->load()) {
            stopping = true;
            continue;
        }
        PeakRunning p;
        if (r.kind == JobKind::Tiles) {
            double     last = 0.0;
            const auto fa = g_tile_ask.find(r.key);
            if (fa != g_tile_ask.end())
                for (const auto& t : r.tiles) {
                    const auto a = fa->second.find(t);
                    if (a != fa->second.end()) last = std::max(last, a->second.asked_s);
                }
            if (now - last > kPeakFreshSeconds) {
                r.cancel->store(true);
                stopping = true;
                continue;
            }
            p.tiles = true;  // 13-57 review: given up first for a clicked file (its tiles are asked again)
            p.asked_s = last;
        } else {
            const auto e = g_cache.find(r.key);
            p.delivered = r.delivered;
            p.asked_s = e != g_cache.end() && e->second.seq == r.seq ? e->second.asked_s : 0.0;
        }
        runs.push_back(p);
        who.push_back(&r);
    }
    // One at a time: the reader it frees takes the waiting file.
    const int k = stopping ? -1 : PickPeakCancel(runs, g_running.size() >= kPeakReaders, fresh_waiting, now);
    if (k >= 0) who[static_cast<size_t>(k)]->cancel->store(true);
}

// 13-57 review: REAPER's peak file of `path` is there and not older than the file (a cheap look on the
// UI thread: its name and its date). False when unknown (the second source is opened, as before).
bool HasPeakFile(const std::string& path)
{
    if (!f_peak_name) return false;
    char buf[4096] = {};
    f_peak_name(path.c_str(), buf, static_cast<int>(sizeof(buf)), false);
    if (!buf[0]) return false;
    try {
        std::error_code             ec;
        const std::filesystem::path pk = std::filesystem::u8path(buf);
        const auto                  tp = std::filesystem::last_write_time(pk, ec);
        if (ec || std::filesystem::file_size(pk, ec) == 0 || ec) return false;
        const auto ts = std::filesystem::last_write_time(std::filesystem::u8path(path), ec);
        return !ec && tp >= ts;
    } catch (...) {
        return false;  // a malformed path
    }
}

// Opens `path` on the UI thread (PCM_Source_CreateFromFile stays here); its time for the log.
PCM_source* OpenSource(const std::string& path, double* ms)
{
    const auto t0 = std::chrono::steady_clock::now();
    PCM_source* s = f_create ? f_create(path.c_str()) : nullptr;
    if (ms) *ms += MsSince(t0);
    return s;
}

// A waiting file's peaks to a free reader: false when none waits.
bool StartPeaksJob(double now)
{
    std::vector<PeakWaiting>                            w;
    std::vector<std::map<std::string, Entry>::iterator> its;
    // 13-57 review: a file whose earlier read is still out (its .reapeaks build) waits for it to end.
    for (auto it = g_cache.begin(); it != g_cache.end(); ++it)
        if (it->second.state == PeakState::Building && !it->second.in_flight && !PeaksRunning(it->first)) {
            w.push_back(PeakWaiting{it->second.seq, it->second.asked_s});
            its.push_back(it);
        }
    if (w.empty()) return false;
    const PeakPick    pick = PickNextPeakJob(w, now);
    const std::string key = pick.next >= 0 ? its[static_cast<size_t>(pick.next)]->first : std::string();
    for (int d : pick.drop) g_cache.erase(its[static_cast<size_t>(d)]);
    if (key.empty()) return false;
    Entry&       e = g_cache[key];
    double       open_ms = 0.0;
    PCM_source*  s = OpenSource(e.path, &open_ms);
    const double sr = s ? s->GetSampleRate() : 0.0;
    const int    nch = s ? s->GetNumChannels() : 0;
    const double len = s ? s->GetLength() : 0.0;
    if (!s || sr < 1.0 || nch < 1 || !(len > 0.0)) {
        delete s;
        e.state = PeakState::Failed;
        return true;
    }
    e.length = len;
    e.sr = sr;
    e.nch = nch;
    g_len[key] = len;
    e.in_flight = true;
    Job j;
    j.id = ++g_job_id;
    j.kind = JobKind::Peaks;
    j.key = key;
    j.seq = e.seq;
    j.src = s;
    // 13-57: a long file gets a second source for its first pass, used only if its peak file is missing
    // (13-57 review: not opened when REAPER's peak file is there, newer than the file).
    if (!PeakReadsSamples(len) && !HasPeakFile(e.path)) j.src2 = OpenSource(e.path, &open_ms);
    j.length = len;
    j.sr = sr;
    j.nch = nch;
    j.open_ms = open_ms;
    j.cancel = std::make_shared<std::atomic<bool>>(false);
    j.pushed = std::chrono::steady_clock::now();
    Running run;
    run.kind = JobKind::Peaks;
    run.key = key;
    run.seq = e.seq;
    run.cancel = j.cancel;
    g_running.emplace(j.id, std::move(run));
    g_reader.Push(std::move(j));
    return true;
}

// The detail tiles waiting, the file asked last first, to a free reader: false when none waits.
bool StartTilesJob(double now)
{
    // The tiles nobody asked for these kPeakFreshSeconds are dropped (the view moved on).
    const std::string* best = nullptr;
    double             best_s = 0.0;
    for (auto f = g_tile_ask.begin(); f != g_tile_ask.end();) {
        for (auto it = f->second.begin(); it != f->second.end();) {
            if (!it->second.in_flight && now - it->second.asked_s > kPeakFreshSeconds) {
                it = f->second.erase(it);
                continue;
            }
            if (!it->second.in_flight && (!best || it->second.asked_s > best_s)) {
                best = &f->first;
                best_s = it->second.asked_s;
            }
            ++it;
        }
        if (f->second.empty()) {
            if (best == &f->first) best = nullptr;  // (cannot happen: a best tile keeps its file)
            f = g_tile_ask.erase(f);
        } else {
            ++f;
        }
    }
    if (!best) return false;
    const std::string key = *best;
    const auto        e = g_cache.find(key);
    if (e == g_cache.end() || e->second.state != PeakState::Ready || !(e->second.sr > 0.0) || e->second.nch < 1) {
        ForgetTiles(key);  // its peaks are gone: asked again, they are read first
        return true;
    }
    // 13-57 review: the tiles asked last first (the zoom the view is at now, not the levels a zoom
    // passed through), PickTilesForJob.
    FileAsks&                 asks = g_tile_ask[key];
    std::vector<WaveTileWant> want;
    std::vector<TileId>       ids;
    for (const auto& kv : asks) {
        if (kv.second.in_flight) continue;
        want.push_back(WaveTileWant{kv.first.first, kv.first.second, kv.second.asked_s});
        ids.push_back(kv.first);
    }
    Job j;
    for (size_t k : PickTilesForJob(want, kTilesPerJob)) j.tiles.push_back(ids[k]);
    double      open_ms = 0.0;
    PCM_source* s = TakeIdleSource(key, e->second.version);
    if (!s) s = OpenSource(e->second.path, &open_ms);
    if (!s) {
        ForgetTiles(key);
        return true;
    }
    for (const auto& t : j.tiles) asks[t].in_flight = true;
    j.id = ++g_job_id;
    j.kind = JobKind::Tiles;
    j.key = key;
    j.src = s;
    j.length = e->second.length;
    j.sr = e->second.sr;
    j.nch = e->second.nch;
    j.version = e->second.version;
    j.open_ms = open_ms;
    j.cancel = std::make_shared<std::atomic<bool>>(false);
    j.pushed = std::chrono::steady_clock::now();
    Running run;
    run.kind = JobKind::Tiles;
    run.key = key;
    run.tiles = j.tiles;
    run.cancel = j.cancel;
    g_running.emplace(j.id, std::move(run));
    g_reader.Push(std::move(j));
    return true;
}

// The free readers get the waiting files' peaks first (the newest asked first, PickNextPeakJob), then
// the detail tiles (never the last reader: a file clicked next starts at once).
void Feed()
{
    if (g_quit) return;
    const double now = NowSeconds();
    CancelUnwanted(now);
    for (int guard = 0; guard < 64 && g_running.size() < kPeakReaders; ++guard) {
        if (StartPeaksJob(now)) continue;
        size_t tile_jobs = 0;
        for (const auto& kv : g_running) tile_jobs += kv.second.kind == JobKind::Tiles ? 1 : 0;
        if (tile_jobs + 1 >= kPeakReaders || !StartTilesJob(now)) break;
    }
}

std::string RootFileOf(PCM_source* src, double* len)
{
    PCM_source* root = src;
    for (int guard = 0; root && root->GetSource() && guard < 8; ++guard) root = root->GetSource();
    if (!root) return {};
    const char* fn = root->GetFileName();
    if (len) *len = root->GetLength();
    return fn ? std::string(fn) : std::string();
}


}  // namespace

void InitSoundPeaks(void* (*get_func)(const char* name))
{
    if (!get_func) return;
    g_quit = false;
    f_create = reinterpret_cast<CreateFromFileFn>(get_func("PCM_Source_CreateFromFile"));
    f_num_tm = reinterpret_cast<GetNumTakeMarkersFn>(get_func("GetNumTakeMarkers"));
    f_get_tm = reinterpret_cast<GetTakeMarkerFn>(get_func("GetTakeMarker"));
    f_chunk = reinterpret_cast<GetItemStateChunkFn>(get_func("GetItemStateChunk"));
    f_count_takes = reinterpret_cast<CountTakesFn>(get_func("CountTakes"));
    f_get_take = reinterpret_cast<GetMediaItemTakeFn>(get_func("GetMediaItemTake"));
    f_takefx_count = reinterpret_cast<TakeFXGetCountFn>(get_func("TakeFX_GetCount"));
    f_peak_name = reinterpret_cast<GetPeakFileNameExFn>(get_func("GetPeakFileNameEx"));
}

void QuitSoundPeaks()
{
    g_quit = true;
    g_reader.Stop();
    g_running.clear();
    g_tile_ask.clear();
    g_tiles.clear();
    for (auto& kv : g_idle) delete kv.second.src;  // 13-57 review: the kept detail sources
    g_idle.clear();
    g_wave_memo.clear();
    for (auto it = g_cache.begin(); it != g_cache.end();)  // an unfinished read is not left waiting
        it = it->second.state == PeakState::Building ? g_cache.erase(it) : std::next(it);
}

PeakState RequestPeaks(const std::string& path, PeakData* out, double* length)
{
    if (path.empty() || !f_create) return PeakState::Failed;
    Collect();
    const std::string key = PeakCacheKey(path);
    auto              it = g_cache.find(key);
    // 13-51: a built (or failed) entry whose file changed on disk (re-rendered at the same path,
    // restored, located) is read again; the disk is asked at most once a second per entry.
    if (it != g_cache.end() && it->second.state != PeakState::Building) {
        const double now = NowSeconds();
        if (FileRecheckDue(it->second.checked_s, now)) {
            it->second.checked_s = now;
            if (FileReadStale(it->second.version, VersionOf(path))) {
                if (g_len.erase(key)) ++g_len_gen;
                ForgetTiles(key);  // 13-57: its detail too
                g_cache.erase(it);
                it = g_cache.end();
            }
        }
    }
    if (it == g_cache.end()) {
        Entry e;
        e.path = path;
        e.seq = ++g_seq;
        e.version = VersionOf(path);  // before the read: a write during it shows as a change after
        e.checked_s = NowSeconds();
        e.first_asked_s = e.checked_s;
        it = g_cache.emplace(key, std::move(e)).first;
    }
    it->second.used = ++g_tick;
    if (it->second.state == PeakState::Building) it->second.asked_s = NowSeconds();
    Feed();  // may hand this file to the reader (or find it does not open)
    it = g_cache.find(key);
    if (it == g_cache.end()) return PeakState::Building;
    const Entry& e = it->second;
    if (e.state == PeakState::Ready && out) {
        *out = e.peaks;
        out->file = e.version;  // 13-54 review: the part trims are keyed on it
    }
    if (length) *length = e.length;
    const PeakState st = e.state;
    Evict();
    return st;
}

FileVersion PeaksFileVersion(const std::string& path)
{
    const auto it = g_cache.find(PeakCacheKey(path));
    return it != g_cache.end() && it->second.state == PeakState::Ready ? it->second.version : FileVersion{};
}

bool ReadPeaks(const std::string& path, double start, double length, double rate, PeakData* out)
{
    if (out) *out = PeakData{};
    if (!out || !(length > 0.0) || !(rate > 0.0)) return false;
    if (RequestPeaks(path, nullptr) != PeakState::Ready) return false;
    auto it = g_cache.find(PeakCacheKey(path));
    if (it == g_cache.end()) return false;
    const PeakData& p = it->second.peaks;
    const double    pr = p.rate > 0.0 ? p.rate : kPeakHiResRate;  // the file's own rate (13-55)
    const size_t    n = static_cast<size_t>(std::max(1.0, std::floor(length * rate)));
    const int       lanes = PeakChannels(p);
    PeakInit(out, lanes, n);
    out->rate = rate;
    for (size_t i = 0; i < n; ++i) {
        const double a = (start + static_cast<double>(i) / rate) * pr;
        const double b = (start + static_cast<double>(i + 1) / rate) * pr;
        int64_t      lo = static_cast<int64_t>(std::floor(a)), hi = static_cast<int64_t>(std::ceil(b));
        if (hi <= lo) hi = lo + 1;
        float m = 0.0f;
        for (int64_t k = std::max<int64_t>(0, lo); k < hi && k < static_cast<int64_t>(p.max_abs.size()); ++k)
            m = std::max(m, p.max_abs[static_cast<size_t>(k)]);
        out->max_abs[i] = m;
        if (lanes == 2) {
            const size_t k0 = static_cast<size_t>(std::max<int64_t>(0, lo));
            const size_t k1 = static_cast<size_t>(std::max<int64_t>(0, hi));
            for (int l = 0; l < 2; ++l) out->lanes[l][i] = PeakLaneMax(p, l, k0, k1);
        }
    }
    return true;
}

bool ReadWaveColumns(const std::string& path, double view_start, double view_len, int columns, WaveColumns* out)
{
    if (!out) return false;
    *out = WaveColumns{};
    if (columns <= 0 || !(view_len > 0.0)) return false;
    if (RequestPeaks(path, nullptr) != PeakState::Ready) return false;
    const std::string key = PeakCacheKey(path);
    const auto        it = g_cache.find(key);
    if (it == g_cache.end()) return false;
    const Entry& e = it->second;
    const double rate = WaveTileRate(view_len, columns, e.sr);
    int64_t      first = 0, last = -1;
    const bool   detail = rate > 0.0 && WaveTilesFor(view_start, view_len, rate, e.length, &first, &last);
    // The tiles the view wants are asked (read on a reader, the newest asked first) and kept fresh in
    // the LRU; meanwhile the levels next to it, then the coarse peaks, draw what they hold (no flicker
    // on a zoom step).
    WaveTileLru::FileTiles* have_file = g_tiles.Of(key);
    if (detail) {
        const double now = NowSeconds();
        FileAsks*    asks = nullptr;
        for (int64_t i = first; i <= last; ++i) {
            const WaveTile* t = have_file ? g_tiles.Get(*have_file, rate, i) : nullptr;
            if (t && t->file == e.version) continue;
            if (!asks) asks = &g_tile_ask[key];
            (*asks)[TileId(rate, i)].asked_s = now;
        }
    }
    // 13-57 review: the waveform is built again only when its view or what it is built from changed
    // (the entry's read, a tile come or gone); else the one built last for this view is given.
    const uint64_t gen = g_tiles.generation();
    WaveMemo*      memo = nullptr;
    for (WaveMemo& m : g_wave_memo)
        if (m.seq == e.seq && m.cols == columns && m.vs == view_start && m.vl == view_len && m.key == key) memo = &m;
    if (memo && memo->gen == gen) {
        memo->used = ++g_memo_tick;
        *out = memo->wc;
        if (detail) Feed();
        return true;
    }
    if (!memo) {
        if (g_wave_memo.size() < kWaveMemos) {
            g_wave_memo.emplace_back();
            memo = &g_wave_memo.back();
        } else {
            memo = &g_wave_memo.front();
            for (WaveMemo& m : g_wave_memo)
                if (m.used < memo->used) memo = &m;
        }
        memo->key = key;
        memo->seq = e.seq;
        memo->vs = view_start;
        memo->vl = view_len;
        memo->cols = columns;
    }
    memo->gen = gen;
    memo->used = ++g_memo_tick;
    WaveColumns coarse;
    ColumnsFromPeaks(e.peaks, view_start, view_len, columns, &coarse);
    if (!detail) {
        memo->wc = coarse;  // a long view: the peaks are at least as fine as its pixels
        *out = std::move(coarse);
        return true;
    }
    // The tiles of the view at each level tried, in a flat list per level (one LRU lookup per tile).
    struct Level {
        double                       rate = 0.0;
        int64_t                      a = 0;
        std::vector<const WaveTile*> t;
    };
    const std::vector<double> rates = WaveFallbackRates(rate, e.sr);
    std::vector<Level>        levels;
    levels.reserve(rates.size());
    for (double r : rates) {
        Level   lv;
        int64_t a = 0, b = -1;
        lv.rate = r;
        if (have_file && WaveTilesFor(view_start, view_len, r, e.length, &a, &b)) {
            lv.a = a;
            lv.t.assign(static_cast<size_t>(b - a + 1), nullptr);
            for (int64_t i = a; i <= b; ++i) {
                const WaveTile* t = g_tiles.Get(*have_file, r, i);
                if (t && t->file == e.version) lv.t[static_cast<size_t>(i - a)] = t;
            }
        }
        levels.push_back(std::move(lv));
    }
    const WaveTileLookup look = [&levels](double r, int64_t i) -> const WaveTile* {
        for (const Level& lv : levels)
            if (lv.rate == r)
                return i >= lv.a && i - lv.a < static_cast<int64_t>(lv.t.size()) ? lv.t[static_cast<size_t>(i - lv.a)] : nullptr;
        return nullptr;
    };
    FoldWaveColumns(look, rates, e.sr, e.nch >= 2 ? kWaveSeries : 1, view_start, view_len, columns, coarse, out, e.length);
    if (!out->line) JoinWaveColumns(out);
    memo->wc = *out;
    Feed();  // the tiles just asked may go to a free reader now
    return true;
}

double SourceFileLength(const std::string& path)
{
    if (path.empty()) return 0.0;
    const std::string key = PeakCacheKey(path);
    auto              it = g_len.find(key);
    if (it != g_len.end()) return it->second;
    double len = 0.0;
    if (f_create) {
        if (PCM_source* s = f_create(path.c_str())) {
            len = s->GetLength();
            delete s;
        }
    }
    if (len > 0.0) g_len[key] = len;  // a missing file is looked for again (restored, located)
    return len;
}

FileVersion SourceFileVersion(const std::string& path)
{
    return path.empty() ? FileVersion{} : VersionOf(path);
}

void ForgetSourceFileLength(const std::string& path)
{
    if (g_len.erase(PeakCacheKey(path))) ++g_len_gen;
}

uint64_t SourceLengthGeneration()
{
    return g_len_gen;
}

double ItemTakeGainDb(MediaItem* item, MediaItem_Take* take)
{
    if (!item) return 0.0;
    double g = GetMediaItemInfo_Value(item, "D_VOL");
    if (take) g *= std::fabs(GetMediaItemTakeInfo_Value(take, "D_VOL"));  // < 0 = polarity flipped
    return g > 0.0 ? 20.0 * std::log10(g) : -60.0;
}

double ItemGainDb(MediaItem* item)
{
    return item ? ItemTakeGainDb(item, GetActiveTake(item)) : 0.0;
}

bool ReadItemMedia(MediaItem* item, SoundMedia* out)
{
    if (!out) return false;
    *out = SoundMedia{};
    if (!item) return false;
    MediaItem_Take* active = GetActiveTake(item);
    out->item_pos = GetMediaItemInfo_Value(item, "D_POSITION");
    out->facts.item_len = GetMediaItemInfo_Value(item, "D_LENGTH");
    out->facts.item_db = ItemGainDb(item);  // the item's gain: its variations' (DetectFrames)
    const int n = f_count_takes ? f_count_takes(item) : (active ? 1 : 0);
    for (int k = 0; k < n; ++k) {
        MediaItem_Take* t = f_get_take ? f_get_take(item, k) : active;
        double          len = 0.0;
        PCM_source*     src = t ? GetMediaItemTake_Source(t) : nullptr;
        std::string     path = src ? RootFileOf(src, &len) : std::string();
        out->take_paths.push_back(path);
        out->take_lens.push_back(len);
        if (t == active) out->active_take = k;
        if (t) {  // 13-49 review: what a placed copy drops (it plays its file plain)
            const double rt = GetMediaItemTakeInfo_Value(t, "D_PLAYRATE");
            out->drops_take_fx = out->drops_take_fx || (f_takefx_count && f_takefx_count(t) > 0);
            out->drops_pitch = out->drops_pitch || std::fabs(GetMediaItemTakeInfo_Value(t, "D_PITCH")) > 1e-9;
            out->drops_rate = out->drops_rate || (std::isfinite(rt) && rt > 0.0 && std::fabs(rt - 1.0) > 1e-9);
            out->drops_pan = out->drops_pan || std::fabs(GetMediaItemTakeInfo_Value(t, "D_PAN")) > 1e-9;
        }
        if (n >= 2) {
            SoundFrame fr;
            fr.start = t ? GetMediaItemTakeInfo_Value(t, "D_STARTOFFS") : 0.0;
            // 13-24 review: the item's length in this take's source time (its playrate), inside its
            // source; 0 = the item's length (DetectFrames).
            const double rate = t ? GetMediaItemTakeInfo_Value(t, "D_PLAYRATE") : 1.0;
            double       fl = out->facts.item_len * ((std::isfinite(rate) && rate > 0.0) ? rate : 1.0);
            if (len > 0.0) fl = std::min(fl, len - fr.start);
            fr.length = fl > 0.0 ? fl : 0.0;
            fr.db = ItemTakeGainDb(item, t);  // the item x this take: its variation's (DetectFrames)
            out->facts.takes.push_back(fr);
        }
    }
    if (!active || out->take_paths.empty()) return false;
    out->path = out->take_paths[static_cast<size_t>(out->active_take)];
    out->source_len = out->take_lens[static_cast<size_t>(out->active_take)];
    out->facts.source_len = out->source_len;
    out->facts.start_offs = GetMediaItemTakeInfo_Value(active, "D_STARTOFFS");
    out->chan_mode = static_cast<int>(GetMediaItemTakeInfo_Value(active, "I_CHANMODE"));
    out->facts.playrate = GetMediaItemTakeInfo_Value(active, "D_PLAYRATE");
    if (!std::isfinite(out->facts.playrate) || !(out->facts.playrate > 0.0)) out->facts.playrate = 1.0;
    // Slices: the take markers, else the chunk's text (spike 13-0 A).
    if (f_num_tm && f_get_tm) {
        std::vector<TakeMarkerPos> marks;
        const int                  m = f_num_tm(active);
        for (int k = 0; k < m; ++k) {
            char name[256] = {};
            TakeMarkerPos p;
            p.pos = f_get_tm(active, k, name, sizeof(name), nullptr);
            p.name = name;
            if (p.pos >= 0.0) marks.push_back(p);
        }
        std::sort(marks.begin(), marks.end(), [](const TakeMarkerPos& a, const TakeMarkerPos& b) { return a.pos < b.pos; });
        if (marks.size() >= 2) out->facts.slices = SlicesFromTakeMarkers(marks, out->source_len);
    }
    if (out->facts.slices.size() < 2 && f_chunk) {
        // 13-51: the shared growing reader (no 1 MB zero-fill per read, a bigger chunk not cut).
        const std::string chunk = ReadItemStateChunk(f_chunk, item);
        if (!chunk.empty()) {
            const std::vector<Slice> s = SlicesFromRppText(chunk);
            if (s.size() >= 2) out->facts.slices = s;
        }
    }
    out->ok = !out->path.empty();
    return out->ok;
}

namespace {
std::function<std::string(const SoundSource&)> g_file_resolver;
}

void SetFileSoundResolver(std::function<std::string(const SoundSource&)> resolver)
{
    g_file_resolver = std::move(resolver);
}

bool ReadSoundMedia(const SoundSource& source, SoundMedia* out)
{
    if (!out) return false;
    *out = SoundMedia{};
    if (source.kind == SoundSourceKind::Item) {
        MediaItem* item = source.item_guid.empty() ? nullptr : FindItemByGuid(source.item_guid);
        return item ? ReadItemMedia(item, out) : false;
    }
    std::string path = source.asset.last_path;
    double      len = SourceFileLength(path);
    if (!(len > 0.0) && g_file_resolver) {  // moved: found where the missing check finds it
        path = g_file_resolver(source);
        len = path.empty() ? 0.0 : SourceFileLength(path);
    }
    if (!(len > 0.0)) return false;
    out->path = path;
    out->source_len = len;
    out->take_paths = {path};
    out->take_lens = {len};
    out->facts.source_len = len;
    out->facts.item_len = len;
    out->facts.item_db = source.item_db;  // the item it was imported from (DetectFrames)
    out->ok = true;
    return true;
}

std::string VariationPath(const SoundMedia& media, const SoundData& data, int variation)
{
    if (data.mode == DetectMode::Takes && variation >= 0 && static_cast<size_t>(variation) < media.take_paths.size())
        return media.take_paths[static_cast<size_t>(variation)];
    return media.path;
}

std::string SoundSourcePath(const SoundSource& source)
{
    SoundMedia m;
    return ReadSoundMedia(source, &m) ? m.path : std::string();
}

}  // namespace rav

#endif  // _WIN32
