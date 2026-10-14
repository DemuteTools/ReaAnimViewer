// SPDX-License-Identifier: MIT
//
// See sound_peaks.h. 13-55: the one peak reader of the Sound panel, Edit and the Media Explorer
// (13-7c's own reader folded in). Threads: the UI thread opens a file (PCM_Source_CreateFromFile)
// and deletes its source; one reader thread, given that source alone, makes sure REAPER's .reapeaks
// file is there (PeaksBuild_Begin / Run / Finish: read when it is there, built and written as REAPER
// does when not) and reads the peaks (GetPeakInfo for a long file, GetSamples for a short one,
// sound_peaks_plan.h). One file at a time; the UI thread polls the results in RequestPeaks (a map
// lookup and a locked swap: no REAPER call per frame) and hands the next waiting file over.

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
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

#include "console_log.h"
#include "file_change_check.h"
#include "reaper_item_read.h"
#include "sound_peaks_plan.h"
#include "sound_populate.h"

namespace rav {
namespace {

using CreateFromFileFn = PCM_source* (*)(const char*);
using GetNumTakeMarkersFn = int (*)(MediaItem_Take*);
using GetTakeMarkerFn = double (*)(MediaItem_Take*, int, char*, int, int*);
using GetItemStateChunkFn = bool (*)(MediaItem*, char*, int, bool);
using CountTakesFn = int (*)(MediaItem*);
using GetMediaItemTakeFn = MediaItem_Take* (*)(MediaItem*, int);
using TakeFXGetCountFn = int (*)(MediaItem_Take*);

CreateFromFileFn    f_create = nullptr;
GetNumTakeMarkersFn f_num_tm = nullptr;
GetTakeMarkerFn     f_get_tm = nullptr;
GetItemStateChunkFn f_chunk = nullptr;
CountTakesFn        f_count_takes = nullptr;
GetMediaItemTakeFn  f_get_take = nullptr;
TakeFXGetCountFn    f_takefx_count = nullptr;  // 13-49 review: optional (the "Placed without" line)

constexpr size_t kCacheFiles = 32;
constexpr int    kBlock = 8192;      // samples per GetSamples call (a short file)
constexpr int    kPeakChunk = 4096;  // points per GetPeakInfo call (a long file)

struct Entry {
    std::string path;  // as first asked: the file is opened with it
    PeakState   state = PeakState::Building;
    PeakData    peaks;
    double      length = 0.0;
    uint64_t    used = 0;
    uint64_t    seq = 0;            // the order it was asked in: its read's id
    double      asked_s = 0.0;      // Building: when it was last asked (NowSeconds)
    bool        in_flight = false;  // Building: the reader thread has it
    FileVersion version;            // 13-51: the file's date + size before it was read
    double      checked_s = 0.0;    // when the disk was last asked (NowSeconds)
};

// The reader thread's work: a source the UI thread opened, handed back (the UI thread deletes it).
struct Job {
    std::string key;
    uint64_t    seq = 0;
    PCM_source* src = nullptr;
    double      length = 0.0, sr = 0.0;
    int         nch = 0;
};
struct JobResult {
    std::string key;
    uint64_t    seq = 0;
    PCM_source* src = nullptr;
    bool        ok = false;
    bool        abandoned = false;  // 13-55 review: given up for another file (it waits again)
    PeakData    peaks;
    PeakReadPath how = PeakReadPath::Samples;
    double       ms = 0.0;  // the read's time (debuglog builds say it)
};

std::atomic<bool> g_quit{false};
// 13-55 review: the seq of the read the UI thread gave up (0 = none): the reader stops it between two
// steps of its build or its read.
std::atomic<uint64_t> g_abandon_seq{0};

// Never destroyed: a building entry's REAPER source is not deleted from static destructors at
// unload (the process is going away; REAPER may already be gone).
std::map<std::string, Entry>& g_cache = *new std::map<std::string, Entry>();  // by PeakCacheKey
uint64_t                      g_tick = 0;
uint64_t                      g_seq = 0;
bool                          g_reading = false;  // a read is out on the reader thread
std::map<std::string, double> g_len;  // by PeakCacheKey (13-55 review), as g_cache
uint64_t                      g_len_gen = 0;  // bumped when a g_len entry is dropped

double NowSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
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

void Evict()
{
    while (g_cache.size() > kCacheFiles) {
        auto oldest = g_cache.end();
        for (auto it = g_cache.begin(); it != g_cache.end(); ++it)
            if (it->second.state != PeakState::Building && (oldest == g_cache.end() || it->second.used < oldest->second.used))
                oldest = it;
        if (oldest == g_cache.end()) return;
        g_cache.erase(oldest);
    }
}

// ---- The reader thread (it only calls the source it was handed) ----------------------------------

// The read in progress stops: unload, or given up for another file.
bool Stopped(const Job& j)
{
    return g_quit || g_abandon_seq.load() == j.seq;
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

// A short file (or REAPER gave no peaks): its samples at kPeakHiResRate (13-7b's read).
bool ReadSamples(PCM_source* s, const Job& j, PeakData* out)
{
    const int64_t total = static_cast<int64_t>(std::ceil(j.length * j.sr));
    out->rate = kPeakHiResRate;
    PeakInit(out, j.nch, PeakBins(j.length, kPeakHiResRate));  // lanes: stereo only
    std::vector<ReaSample> buf(static_cast<size_t>(kBlock) * static_cast<size_t>(j.nch));
    for (int64_t done = 0; done < total;) {
        if (Stopped(j)) return false;
        PCM_source_transfer_t tr = {};
        tr.time_s = static_cast<double>(done) / j.sr;
        tr.samplerate = j.sr;
        tr.nch = j.nch;
        tr.length = static_cast<int>(std::min<int64_t>(kBlock, total - done));
        tr.samples = buf.data();
        s->GetSamples(&tr);
        const int got = tr.samples_out > 0 ? std::min(tr.samples_out, tr.length) : 0;
        for (int k = 0; k < got; ++k) {
            const size_t bin = static_cast<size_t>(static_cast<double>(done + k) * kPeakHiResRate / j.sr);
            PeakAddFrame(out, bin, buf.data() + static_cast<size_t>(k) * static_cast<size_t>(j.nch), j.nch);
        }
        done += tr.length;  // a short read is silence: never stall on it
    }
    return true;
}

// A short file (up to 10 s) is read from its samples alone: no peak file is built for it (13-55
// review). A long one: REAPER's own peak file first (PeaksBuild_Begin answers 0 when it is there and
// up to date; else it is built and written where REAPER keeps its peaks, as REAPER does for a file it
// plays), then its peaks; its samples only when REAPER gave no peak point at all.
//
// A build stopped part way (unload, or given up for another file) is NOT finished: the SDK only says
// PeaksBuild_Finish is "called when done", not whether it would keep a part-built peak file as
// complete, so the source is deleted with its build open and nothing is deleted on disk (13-55
// review); the next read of that file builds it again.
bool ReadJob(const Job& j, PeakData* out, PeakReadPath* how)
{
    PCM_source* s = j.src;
    *how = PeakReadPath::Samples;
    if (PeakReadsSamples(j.length)) return ReadSamples(s, j, out);
    *how = PeakReadPath::PeakFile;
    if (s->PeaksBuild_Begin()) {
        *how = PeakReadPath::BuiltThenPeakFile;
        bool more = true;
        while (more && !Stopped(j)) more = s->PeaksBuild_Run() != 0;
        if (more) return false;  // stopped mid-build: not finished (above)
        s->PeaksBuild_Finish();
    }
    if (Stopped(j)) return false;
    if (ReadPeakInfo(s, j, ChoosePeakRate(j.length), out)) return true;
    if (Stopped(j)) return false;
    *how = PeakReadPath::SamplesFallback;
    *out = PeakData{};
    return ReadSamples(s, j, out);
}

// One thread, one read at a time (the UI thread hands it the next only once this one came back).
class PeakReader {
public:
    void Push(Job j)
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            q_.push_back(std::move(j));
            if (!th_.joinable()) th_ = std::thread([this] { Run(); });
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
    // Unload: the read in progress stops (g_quit), the thread is joined, every source deleted.
    void Stop()
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
        for (JobResult& r : Take()) delete r.src;
        std::lock_guard<std::mutex> lk(m_);
        for (Job& j : q_) delete j.src;
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
            r.key = j.key;
            r.seq = j.seq;
            r.src = j.src;
            // Nothing may escape this thread (a throw would end REAPER): a failed read still answers.
            const auto t0 = std::chrono::steady_clock::now();
            try {
                r.ok = j.src && ReadJob(j, &r.peaks, &r.how);
            } catch (...) {
                r.ok = false;
            }
            r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            r.abandoned = !r.ok && !g_quit && g_abandon_seq.load() == j.seq;
            if (!r.ok) r.peaks = PeakData{};
            std::lock_guard<std::mutex> lk(m_);
            out_.push_back(std::move(r));
        }
    }

    std::thread             th_;
    std::mutex              m_;
    std::condition_variable cv_;
    std::deque<Job>         q_;
    std::vector<JobResult>  out_;
    bool                    stop_ = false;
};

PeakReader& g_reader = *new PeakReader();  // never destroyed: joined by QuitSoundPeaks

// ---- The UI thread's side ------------------------------------------------------------------------

// The reads that came back: their entries are Ready (or Failed); every source is deleted here.
void Collect()
{
    for (JobResult& r : g_reader.Take()) {
        delete r.src;
        g_reading = false;
        if (g_abandon_seq.load() == r.seq) g_abandon_seq = 0;  // asked again, the file is read again
        auto it = g_cache.find(r.key);
        if (it == g_cache.end() || it->second.seq != r.seq) continue;  // dropped meanwhile
        Entry& e = it->second;
        e.in_flight = false;
        if (r.abandoned) continue;  // 13-55 review: waits again (dropped if nobody asks for it)
#ifdef RAV_ENABLE_CONSOLE_LOG
        // 13-55 review (debuglog builds only): which way this file's read went, and its time.
        if (r.ok) LogInfo("peaks: %s: %s, %.0f ms", e.path.c_str(), PeakReadPathName(r.how), r.ms);
#endif
        e.state = r.ok ? PeakState::Ready : PeakState::Failed;
        if (r.ok) e.peaks = std::move(r.peaks);
        e.used = ++g_tick;  // just read: not the first evicted before its asker takes it
    }
}

// When the reader is free: the waiting files nobody asks for any more are dropped, and the one to
// read next is opened here (PCM_Source_CreateFromFile stays on the UI thread) and handed over.
void Feed()
{
    if (g_quit) return;
    if (g_reading) {
        // 13-55 review: the file on the reader nobody asks for any more while another one waits
        // (asked just now) is given up, so the wanted file is read next (PeakAbandonInFlight).
        const double now = NowSeconds();
        const Entry* busy = nullptr;
        bool         other_fresh = false;
        for (const auto& kv : g_cache) {
            const Entry& e = kv.second;
            if (e.state != PeakState::Building) continue;
            if (e.in_flight) busy = &e;
            else if (now >= e.asked_s && now - e.asked_s <= kPeakFreshSeconds) other_fresh = true;
        }
        if (busy && PeakAbandonInFlight(busy->asked_s, other_fresh, now)) g_abandon_seq = busy->seq;
        return;
    }
    std::vector<PeakWaiting>                            w;
    std::vector<std::map<std::string, Entry>::iterator> its;
    for (auto it = g_cache.begin(); it != g_cache.end(); ++it)
        if (it->second.state == PeakState::Building && !it->second.in_flight) {
            w.push_back(PeakWaiting{it->second.seq, it->second.asked_s});
            its.push_back(it);
        }
    if (w.empty()) return;
    const PeakPick    pick = PickNextPeakJob(w, NowSeconds());
    const std::string key = pick.next >= 0 ? its[static_cast<size_t>(pick.next)]->first : std::string();
    for (int d : pick.drop) g_cache.erase(its[static_cast<size_t>(d)]);
    if (key.empty()) return;
    Entry&       e = g_cache[key];
    PCM_source*  s = f_create ? f_create(e.path.c_str()) : nullptr;
    const double sr = s ? s->GetSampleRate() : 0.0;
    const int    nch = s ? s->GetNumChannels() : 0;
    const double len = s ? s->GetLength() : 0.0;
    if (!s || sr < 1.0 || nch < 1 || !(len > 0.0)) {
        delete s;
        e.state = PeakState::Failed;
        return;
    }
    e.length = len;
    g_len[key] = len;
    e.in_flight = true;
    Job j;
    j.key = key;
    j.seq = e.seq;
    j.src = s;
    j.length = len;
    j.sr = sr;
    j.nch = nch;
    g_reading = true;
    g_reader.Push(std::move(j));
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
}

void QuitSoundPeaks()
{
    g_quit = true;
    g_reader.Stop();
    g_reading = false;
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
