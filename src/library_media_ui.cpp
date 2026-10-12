// SPDX-License-Identifier: MIT
//
// See library_media_ui.h. Spec 13-7c (DECISIONS section 6, mockup renderMediaExplorer / useFiles).
//
// Threads: the database load is media_db's worker (MediaDbStartLoad). This file has two more:
// the probe worker (does a file exist / can it be reached: plain std::filesystem, exists and
// size, no REAPER call) and the peak reader (only GetSamples on a source the UI thread made). Every
// REAPER call (PCM_Source_CreateFromFile, deleting a source) stays on the UI thread, made only once
// the probe answered that the file is reachable, so an offline network share only leaves rows
// "checking...", the preview "opening..." or a use "Reading N files...". Results come back
// through a locked queue the draw polls; the UI thread deletes every source it takes back (stale or
// not); both workers are joined by QuitLibraryMedia, which deletes the sources nobody took.
// No folder is ever walked here.

#include "library_media_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "library_media_plan.h"
#include "library_tree_ui.h"
#include "library_view_ui.h"
#include "media_db.h"
#include "reaper_api.h"
#include "shortcuts.h"
#include "sound_audition.h"
#include "sound_entries.h"
#include "sound_project.h"
#include "sound_record.h"
#include "ui_theme.h"

namespace rav {

namespace {

using PlayPreviewFn = int (*)(preview_register_t* preview);
using StopPreviewFn = int (*)(preview_register_t* preview);
using CreateFromFileFn = PCM_source* (*)(const char* filename);

PlayPreviewFn    f_play = nullptr;
StopPreviewFn    f_stop = nullptr;
CreateFromFileFn f_create = nullptr;

const char* const kPayload = "RAV_MEDIA";
const char* const kAutoPref = "lib_media_autoplay";
constexpr size_t  kMaxResults = 1000;
constexpr double  kDebounce = 0.2;
constexpr double  kPeakRate = 200.0;

// ---- A clip of a file: [start, end) of the inner source, silence after (decision 10) -----------

class ClipSource : public PCM_source {
public:
    ClipSource(PCM_source* inner, double start, double end) : inner_(inner), start_(start), len_(std::max(0.0, end - start)) {}
    ~ClipSource() override { delete inner_; }
    PCM_source* Duplicate() override
    {
        PCM_source* d = inner_ ? inner_->Duplicate() : nullptr;
        return d ? new ClipSource(d, start_, start_ + len_) : nullptr;
    }
    bool        IsAvailable() override { return inner_ && inner_->IsAvailable(); }
    const char* GetType() override { return "RAVCLIP"; }
    const char* GetFileName() override { return inner_ ? inner_->GetFileName() : nullptr; }
    bool        SetFileName(const char*) override { return false; }
    int         GetNumChannels() override { return inner_ ? inner_->GetNumChannels() : 0; }
    double      GetSampleRate() override { return inner_ ? inner_->GetSampleRate() : 0.0; }
    double      GetLength() override { return len_; }
    int         PropertiesWindow(HWND) override { return 0; }
    void        GetSamples(PCM_source_transfer_t* block) override
    {
        if (!block) return;
        const int    nch = std::max(1, block->nch);
        const int    want = std::max(0, block->length);
        int          got = 0;
        const double left = len_ - block->time_s;
        if (inner_ && left > 0.0 && block->time_s >= 0.0 && block->samplerate > 0.0) {
            int n = static_cast<int>(std::min<double>(want, std::floor(left * block->samplerate)));
            if (n > 0) {
                PCM_source_transfer_t t = *block;
                t.time_s = start_ + block->time_s;
                t.length = n;
                t.samples_out = 0;
                inner_->GetSamples(&t);
                got = std::max(0, std::min(n, t.samples_out));
            }
        }
        if (block->samples && got < want)
            std::memset(block->samples + static_cast<size_t>(got) * nch, 0,
                        static_cast<size_t>(want - got) * nch * sizeof(ReaSample));
        block->samples_out = want;
    }
    void GetPeakInfo(PCM_source_peaktransfer_t* block) override
    {
        if (block) block->peaks_out = 0;
    }
    void SaveState(ProjectStateContext*) override {}
    int  LoadState(const char*, ProjectStateContext*) override { return -1; }
    void Peaks_Clear(bool) override {}
    int  PeaksBuild_Begin() override { return 0; }
    int  PeaksBuild_Run() override { return 0; }
    void PeaksBuild_Finish() override {}

private:
    PCM_source* inner_ = nullptr;
    double      start_ = 0.0;
    double      len_ = 0.0;
};

// ---- Disk workers --------------------------------------------------------------------------------

// Exists: a row's missing check. Reach: the file is there and answers a short read (then the UI
// thread opens it). Peaks: GetSamples over a source the UI thread opened.
enum class TaskKind { Exists, Reach, Peaks };
enum class PeakOwner { Open, Preview, Batch };  // Open: the preview's playback (Reach only)

struct Task {
    TaskKind    kind = TaskKind::Exists;
    std::string path;
    uint64_t    id = 0;  // its owner's generation
    PeakOwner   owner = PeakOwner::Preview;
    PCM_source* src = nullptr;  // Peaks: made and deleted by the UI thread
};

struct TaskResult {
    TaskKind    kind = TaskKind::Exists;
    std::string path;
    uint64_t    id = 0;
    PeakOwner   owner = PeakOwner::Preview;
    bool        ok = false;       // Exists / Reach: the file is there; Peaks: it was read
    bool        done = true;      // Peaks: false when stopped (stale)
    PCM_source* src = nullptr;    // Peaks: handed back, the UI thread deletes it
    PeakData    peaks;
    double      length = 0.0;
};

// A newer request makes an older one stale: a stale Open deletes its source, a stale read stops.
std::atomic<uint64_t> g_open_gen{0};
std::atomic<uint64_t> g_preview_gen{0};
std::atomic<uint64_t> g_batch_gen{0};
std::atomic<bool>     g_quitting{false};

bool TaskStale(const Task& t)
{
    if (g_quitting) return true;
    if (t.kind == TaskKind::Exists) return false;
    switch (t.owner) {
    case PeakOwner::Open: return t.id != g_open_gen;
    case PeakOwner::Preview: return t.id != g_preview_gen;
    default: return t.id != g_batch_gen;
    }
}

// The whole file's peaks (max |sample| over the channels) at ~kPeakRate; false when cancelled.
bool ReadFilePeaks(PCM_source* s, const Task& t, PeakData* peaks, double* length)
{
    const int    nch = s->GetNumChannels();
    const double sr = s->GetSampleRate();
    const double len = s->GetLength();
    if (nch < 1 || sr < 1.0 || !(len > 0.0)) return true;
    const int     per = std::max(1, static_cast<int>(std::lround(sr / kPeakRate)));
    const int     block = per * 64;
    const int64_t total = static_cast<int64_t>(len * sr);
    std::vector<ReaSample> buf(static_cast<size_t>(block) * nch);
    peaks->rate = sr / per;
    peaks->max_abs.reserve(static_cast<size_t>(total / per + 1));
    float cur = 0.0f;
    int   count = 0;
    for (int64_t pos = 0; pos < total; pos += block) {
        if (TaskStale(t)) return false;
        PCM_source_transfer_t tr{};
        tr.time_s = pos / sr;
        tr.samplerate = sr;
        tr.nch = nch;
        tr.length = static_cast<int>(std::min<int64_t>(block, total - pos));
        tr.samples = buf.data();
        s->GetSamples(&tr);
        const int out = std::max(0, std::min(tr.length, tr.samples_out));
        for (int i = 0; i < tr.length; ++i) {
            float m = 0.0f;
            if (i < out)
                for (int c = 0; c < nch; ++c) m = std::max(m, static_cast<float>(std::fabs(buf[static_cast<size_t>(i) * nch + c])));
            cur = std::max(cur, m);
            if (++count == per) {
                peaks->max_abs.push_back(cur);
                cur = 0.0f;
                count = 0;
            }
        }
    }
    if (count > 0) peaks->max_abs.push_back(cur);
    *length = len;
    return true;
}

// One thread running tasks in order (front pushes first); results wait in `out_` for the draw.
class DiskWorker {
public:
    void Push(Task t, bool front)
    {
        if (g_quitting) {
            delete t.src;
            return;
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            if (front) q_.push_front(std::move(t));
            else q_.push_back(std::move(t));
            if (!th_.joinable()) th_ = std::thread([this] { Run(); });
        }
        cv_.notify_one();
    }
    std::vector<TaskResult> Take()
    {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<TaskResult> r;
        r.swap(out_);
        return r;
    }
    // Unload: the queued tasks are dropped, the running one stops (g_quitting), the thread joined,
    // and the sources nobody took are deleted.
    void Stop()
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
            for (Task& t : q_) dropped_.push_back(std::move(t));
            q_.clear();
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
        for (TaskResult& r : Take()) delete r.src;
        std::deque<Task> left;
        {
            std::lock_guard<std::mutex> lk(m_);
            left.swap(dropped_);
        }
        for (Task& t : left) delete t.src;
        std::lock_guard<std::mutex> lk(m_);
        stop_ = false;
    }

private:
    void Run()
    {
        for (;;) {
            Task t;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                if (stop_) return;
                t = std::move(q_.front());
                q_.pop_front();
            }
            // A source always comes back to the UI thread (it deletes it), stale or not.
            if (TaskStale(t) && !t.src) continue;
            TaskResult r;
            r.kind = t.kind;
            r.path = t.path;
            r.id = t.id;
            r.owner = t.owner;
            r.src = t.src;
            // Nothing may escape this thread (a throw would end REAPER): a failed task still answers,
            // so a waiting use never waits forever.
            try {
                if (t.kind == TaskKind::Peaks) {
                    r.done = !TaskStale(t);
                    if (r.done && t.src) r.done = ReadFilePeaks(t.src, t, &r.peaks, &r.length);
                    r.ok = r.done && t.src && r.length > 0.0;
                } else {
                    std::error_code ec;
                    const std::filesystem::path fp = std::filesystem::u8path(t.path);
                    r.ok = std::filesystem::exists(fp, ec);
                    if (r.ok && t.kind == TaskKind::Reach)  // its size answers too: the share is awake
                        r.ok = std::filesystem::file_size(fp, ec) != static_cast<std::uintmax_t>(-1) && !ec;
                }
            } catch (...) {
                r.ok = false;
                r.peaks = PeakData{};
                r.length = 0.0;
            }
            std::lock_guard<std::mutex> lk(m_);
            out_.push_back(std::move(r));
        }
    }

    std::thread             th_;
    std::mutex              m_;
    std::condition_variable cv_;
    std::deque<Task>        q_;
    std::deque<Task>        dropped_;  // queued at Stop: their sources are deleted after the join
    std::vector<TaskResult> out_;
    bool                    stop_ = false;
};

DiskWorker g_probe;   // Exists, Reach (quick, but may stall on an offline share)
DiskWorker g_reader;  // Peaks

// ---- Preview (REAPER's preview API, one sound at a time) -----------------------------------------

struct Preview {
    preview_register_t reg{};
    bool               cs_ready = false;
    bool               playing = false;
    PCM_source*        src = nullptr;
    std::string        path;
    double             offset = 0.0;  // file time of the source's 0 (a clip's start)
    double             length = 0.0;  // the played source's length
    int                frame = -1;    // the variation playing, -1 = the file
    // A start waiting for its source (opened on the probe worker).
    bool               opening = false;
    std::string        open_path;
    double             open_from = 0.0, open_to = -1.0;
    int                open_frame = -1;
};
Preview g_pv;

void EnsureCs()
{
    if (g_pv.cs_ready) return;
    InitializeCriticalSection(&g_pv.reg.cs);
    g_pv.cs_ready = true;
}

void StopPreviewNow()
{
    if (g_pv.opening) {
        g_pv.opening = false;
        ++g_open_gen;  // its reach answer is ignored
    }
    if (!g_pv.playing) return;
    if (f_stop) f_stop(&g_pv.reg);
    delete g_pv.src;
    g_pv.src = nullptr;
    g_pv.reg.src = nullptr;
    g_pv.playing = false;
    g_pv.frame = -1;
}

// Plays `path` from `from` (file seconds); to > from: only [from, to) (a variation). The source
// is reached on the probe worker ("opening..."), then opened here and played by FinishPreview.
void StartPreview(const std::string& path, double from, double to, int frame)
{
    StopAudition();
    StopPreviewNow();
    if (!f_play || !f_stop || !f_create || path.empty()) return;
    g_pv.opening = true;
    g_pv.open_path = path;
    g_pv.open_from = from;
    g_pv.open_to = to;
    g_pv.open_frame = frame;
    Task t;
    t.kind = TaskKind::Reach;
    t.path = path;
    t.id = ++g_open_gen;
    t.owner = PeakOwner::Open;
    g_probe.Push(std::move(t), true);
}

void FinishPreview(PCM_source* file)
{
    g_pv.opening = false;
    const std::string path = g_pv.open_path;
    const double      from = g_pv.open_from, to = g_pv.open_to;
    const int         frame = g_pv.open_frame;
    PCM_source* src = file;
    double      cur = std::max(0.0, from);
    if (to > from) {
        src = new ClipSource(file, from, to);
        cur = 0.0;
    }
    EnsureCs();
    EnterCriticalSection(&g_pv.reg.cs);
    g_pv.reg.src = src;
    g_pv.reg.m_out_chan = 0;
    g_pv.reg.curpos = cur;
    g_pv.reg.loop = false;
    g_pv.reg.volume = 1.0;
    g_pv.reg.peakvol[0] = g_pv.reg.peakvol[1] = 0.0;
    g_pv.reg.preview_track = nullptr;
    LeaveCriticalSection(&g_pv.reg.cs);
    g_pv.src = src;
    g_pv.path = path;
    g_pv.offset = to > from ? from : 0.0;
    g_pv.length = src->GetLength();
    g_pv.frame = frame;
    g_pv.playing = f_play(&g_pv.reg) != 0;
    if (!g_pv.playing) {
        delete src;
        g_pv.src = nullptr;
        g_pv.reg.src = nullptr;
    }
}

// The file time being played, -1 when nothing plays. Ends a preview that reached its end.
double PreviewFileTime()
{
    if (!g_pv.playing) return -1.0;
    EnterCriticalSection(&g_pv.reg.cs);
    double cur = g_pv.reg.curpos;
    LeaveCriticalSection(&g_pv.reg.cs);
    if (cur >= g_pv.length - 1e-4) {
        StopPreviewNow();
        return -1.0;
    }
    return g_pv.offset + cur;
}

// ---- Panel state ---------------------------------------------------------------------------------

struct Analysis {
    std::string             path;
    bool                    ready = false;
    bool                    failed = false;  // ready, but not read (a use reads it again and names it)
    double                  length = -1.0;
    PeakData                peaks;
    std::vector<SoundFrame> frames;
};

struct State {
    bool                                load_started = false;
    bool                                prefs_loaded = false;
    bool                                autoplay = true;
    char                                query[256] = {};
    int                                 filter = 0;  // 0 All, 1..N a database, N+1 Favourites
    double                              edited_at = -1.0;
    bool                                dirty = false;
    std::shared_ptr<const MediaDbIndex> index;      // the one the results point into
    std::vector<const DbFile*>          results;
    bool                                capped = false;
    MediaDbInfo                         info;
    bool                                info_ready = false;
    std::vector<std::string>            favs;
    std::set<std::string>               fav_keys;  // lowercase
    bool                                favs_loaded = false;
    std::unordered_map<std::string, int> probe;   // per path, for the session: 0 checking, 1 found, 2 missing
    std::vector<DbFile>                 sel;        // selected files (copies)
    bool                                has_cur = false;
    DbFile                              cur;        // the previewed file
    std::vector<int>                    vsel;       // its selected variations
    Analysis                            an;
    uint32_t                            drag_gen = 0;
    int                                 drag_frame = -10;
    std::vector<MediaPick>              drag_picks;
    // A use waiting for its files' variations (read on the peak reader, generation g_batch_gen).
    bool                                pending = false;
    MediaBatch                          batch;
    std::string                         pend_lib;
    bool                                pend_has_row = false;
    LibraryRow                          pend_row;
    ReaProject*                         pend_proj = nullptr;  // the project the use was made in
    int                                 last_frame = -1;  // the last frame the panel was drawn
};
State g;

std::string Lower(const std::string& s)
{
    std::string o = s;
    for (char& c : o)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}

std::string ResourcePath()
{
    const char* p = GetResourcePath();
    return p ? std::string(p) : std::string();
}

void LoadFavs()
{
    if (g.favs_loaded) return;
    g.favs_loaded = true;
    g.favs = ReadMediaFavouritesFile(ResourcePath());
    g.fav_keys.clear();
    for (const std::string& f : g.favs) g.fav_keys.insert(Lower(f));
}

bool IsFav(const std::string& path) { return g.fav_keys.count(Lower(path)) != 0; }

void ToggleFav(const std::string& path)
{
    const std::string k = Lower(path);
    if (g.fav_keys.count(k)) {
        g.fav_keys.erase(k);
        g.favs.erase(std::remove_if(g.favs.begin(), g.favs.end(), [&](const std::string& f) { return Lower(f) == k; }),
                     g.favs.end());
    } else {
        g.fav_keys.insert(k);
        g.favs.push_back(path);
    }
    WriteMediaFavouritesFile(ResourcePath(), g.favs);
    if (g.filter == static_cast<int>(g.info.databases.size()) + 1) g.dirty = true;
}

// Decision 8: checked once per path, for the rows shown, on the probe worker (0 = still checking).
int Probe(const std::string& path)
{
    auto it = g.probe.find(path);
    if (it != g.probe.end()) return it->second;
    g.probe[path] = 0;
    Task t;
    t.kind = TaskKind::Exists;
    t.path = path;
    g_probe.Push(std::move(t), false);
    return 0;
}

// Known missing (a file still being checked is not).
bool IsMissing(const std::string& path) { return Probe(path) == 2; }

void RunSearch()
{
    g.dirty = false;
    g.index = MediaDbGet();
    g.results.clear();
    g.capped = false;
    const std::string q = g.query;
    const int         ndb = static_cast<int>(g.info.databases.size());
    const bool        favs = g.filter == ndb + 1;
    bool              blank = true;
    for (char c : q)
        if (c != ' ' && c != '\t') blank = false;
    if (favs) {
        std::vector<const DbFile*> pool;
        if (blank) {
            for (const DbFile& f : g.index->Files()) pool.push_back(&f);
        } else {
            pool = g.index->Search(q, std::string(), g.index->Size());
        }
        for (const DbFile* f : pool)
            if (IsFav(f->path)) g.results.push_back(f);
    } else if (!blank) {
        std::string db = (g.filter >= 1 && g.filter <= ndb) ? g.info.databases[g.filter - 1].name : std::string();
        g.results = g.index->Search(q, db, kMaxResults + 1);
    }
    if (g.results.size() > kMaxResults) {
        g.results.resize(kMaxResults);
        g.capped = true;
    }
}

bool IsSelected(const std::string& path)
{
    for (const DbFile& f : g.sel)
        if (f.path == path) return true;
    return false;
}

// A file's peaks (read, or failed to open / read) for the preview or the waiting use.
void PeakResult(TaskResult& r)
{
    if (r.owner == PeakOwner::Preview) {
        if (r.id != g_preview_gen || r.path != g.an.path) return;
        g.an.peaks = std::move(r.peaks);
        if (r.length > 0.0) g.an.length = r.length;
        g.an.frames = r.ok ? SplitOnSilence(g.an.peaks, SilenceParams{}, g.an.length) : std::vector<SoundFrame>{};
        g.an.failed = !r.ok;
        g.an.ready = true;
        // A double-click previews and uses the same file: the use takes this read, not a second one.
        if (g.pending) MediaBatchSetRead(g.batch, r.path, r.ok, g.an.frames, r.length);
    } else if (r.owner == PeakOwner::Batch && g.pending && r.id == g_batch_gen) {
        std::vector<SoundFrame> frames;
        if (r.ok) frames = SplitOnSilence(r.peaks, SilenceParams{}, r.length);
        MediaBatchSetRead(g.batch, r.path, r.ok, frames, r.length);
    }
}

// Shows a file in the preview (its peaks read on the worker).
void SetCurrent(const DbFile& f)
{
    if (g.has_cur && g.cur.path == f.path) return;
    g.cur = f;
    g.has_cur = true;
    g.vsel.clear();
    g.an = Analysis{};
    g.an.path = f.path;
    g.an.length = f.length;
    const uint64_t gen = ++g_preview_gen;  // the previous file's read stops
    // A missing file (or no REAPER API) has nothing to read: no audio, not "Reading...". A file
    // that does not open comes back failed from the reader.
    if (!f_create || IsMissing(f.path)) {
        g.an.ready = true;
        return;
    }
    Task t;
    t.kind = TaskKind::Reach;
    t.path = f.path;
    t.id = gen;
    t.owner = PeakOwner::Preview;
    g_probe.Push(std::move(t), true);  // then read before any file of a waiting use
}

// A file reached: opened here (UI thread), its peaks read on the reader.
void ReadReached(const TaskResult& r, bool front)
{
    Task t;
    t.kind = TaskKind::Peaks;
    t.path = r.path;
    t.id = r.id;
    t.owner = r.owner;
    t.src = f_create ? f_create(r.path.c_str()) : nullptr;
    if (!t.src) {  // did not open: answered at once, not read
        TaskResult f;
        f.kind = TaskKind::Peaks;
        f.path = r.path;
        f.id = r.id;
        f.owner = r.owner;
        f.ok = false;
        PeakResult(f);
        return;
    }
    g_reader.Push(std::move(t), front);
}

void CommitPending();

// The workers' results: rows found or missing, the preview's source, the peaks read.
void PollWorkers()
{
    for (TaskResult& r : g_probe.Take()) {
        auto it = g.probe.find(r.path);
        if (it != g.probe.end()) it->second = r.ok ? 1 : 2;
        else if (r.kind == TaskKind::Reach) g.probe[r.path] = r.ok ? 1 : 2;
        if (r.kind == TaskKind::Exists) {
            if (!r.ok && g.has_cur && g.cur.path == r.path && !g.an.ready) {
                ++g_preview_gen;  // found missing while its peaks wait: no audio
                g.an.ready = true;
            }
            continue;
        }
        if (r.owner == PeakOwner::Open) {
            if (!g_pv.opening || r.id != g_open_gen) continue;
            PCM_source* src = r.ok && f_create ? f_create(r.path.c_str()) : nullptr;
            if (src) FinishPreview(src);
            else g_pv.opening = false;  // missing, or it did not open
        } else if (r.owner == PeakOwner::Preview) {
            if (r.id != g_preview_gen || r.path != g.an.path) continue;
            if (r.ok) ReadReached(r, true);
            else g.an.ready = true;  // missing: no audio
        } else if (g.pending && r.id == g_batch_gen) {
            if (r.ok) ReadReached(r, false);
            else MediaBatchSetMissing(g.batch, r.path);  // dropped from the use, named in the toast
        }
    }
    for (TaskResult& r : g_reader.Take()) {
        PCM_source* src = r.src;  // the UI thread deletes every source it takes back
        r.src = nullptr;
        delete src;
        if (r.kind == TaskKind::Peaks && r.done) PeakResult(r);
    }
    if (g.pending && MediaBatchLeft(g.batch) == 0) CommitPending();
}

MediaPick WholePick(const DbFile& f)
{
    MediaPick p;
    p.path = f.path;
    p.size = f.size;
    p.length = f.length;
    p.mtime = f.mtime;
    if (g.has_cur && g.cur.path == f.path && g.an.ready && !g.an.failed) {
        p.frames = g.an.frames;
        p.frames_known = true;
        if (g.an.length > 0.0) p.length = g.an.length;
    }
    return p;
}

std::vector<MediaPick> SlicePicks(const std::vector<int>& idx)
{
    std::vector<MediaPick> out;
    if (!g.has_cur) return out;
    std::vector<int> sorted = idx;
    std::sort(sorted.begin(), sorted.end());
    for (int i : sorted) {
        if (i < 0 || i >= static_cast<int>(g.an.frames.size())) continue;
        MediaPick p = WholePick(g.cur);
        p.frames.clear();
        p.slice = true;
        p.index = i;
        p.frame = g.an.frames[i];
        out.push_back(p);
    }
    return out;
}

// Decision 12: the selected variations of the previewed file, else the selected files (whole).
std::vector<MediaPick> UsedPicks(bool* variations)
{
    *variations = false;
    if (g.has_cur && !g.vsel.empty() && g.an.ready) {
        *variations = true;
        return SlicePicks(g.vsel);
    }
    std::vector<MediaPick> out;
    for (const DbFile& f : g.sel)
        if (!IsMissing(f.path)) out.push_back(WholePick(f));
    return out;
}

// The Sounds of every pick, one undo point. `suffix` names the files that could not be read.
void ApplyPicks(const std::string& lib, const LibraryRow* row, const std::vector<MediaPick>& picks,
                const std::string& suffix)
{
    if (picks.empty()) return;
    std::string toast, why;
    std::vector<std::string> created;
    const char* desc = MediaUseAddsInside(GetSoundStore(nullptr), lib, row) ? "RAV: Add sounds"
                                                                             : "RAV: Set sound from Media Explorer";
    bool ok = ModifySoundStore(nullptr, desc, [&](SoundStore& s) {
        return UseMediaAt(s, lib, row, picks, &created, &toast, &why);
    });
    if (ok) {
        ShowLibraryToast(toast + suffix, true, desc);
        g.vsel.clear();
    } else if (!why.empty()) {
        ShowLibraryToast(why, false);
    }
}

// Drops the waiting use (its reads stop).
void CancelPending()
{
    if (!g.pending) return;
    g.pending = false;
    g.batch = MediaBatch{};
    ++g_batch_gen;
}

void CommitPending()
{
    if (!g.pending) return;
    MediaBatch        b = std::move(g.batch);
    const std::string lib = g.pend_lib;
    const LibraryRow  row = g.pend_row;
    const bool        has_row = g.pend_has_row;
    ReaProject* const proj = g.pend_proj;
    CancelPending();
    if (EnumProjects(-1, nullptr, 0) != proj) {  // another project tab since the use
        ShowLibraryToast("Use cancelled: the project changed", false);
        return;
    }
    const std::string suffix = MediaFailedToastSuffix(MediaBatchFailedNames(b)) +
                               MediaMissingToastSuffix(MediaBatchMissingNames(b));
    const std::vector<MediaPick> used = MediaBatchUsedPicks(b);
    if (used.empty()) {
        ShowLibraryToast("Nothing added" + suffix, false);
        return;
    }
    ApplyPicks(lib, has_row ? &row : nullptr, used, suffix);
}

// A use: every whole file whose variations are not known yet is read first ("Reading N
// files..."), then all the Sounds are made at once. A new use replaces a waiting one.
void UsePicks(const std::string& lib, const LibraryRow* row, const std::vector<MediaPick>& picks)
{
    CancelPending();
    if (picks.empty()) return;
    MediaBatch b = StartMediaBatch(picks);
    const std::vector<std::string> paths = MediaBatchPathsToRead(b);
    if (paths.empty()) {
        ApplyPicks(lib, row, b.picks, std::string());
        return;
    }
    g.pending = true;
    g.batch = std::move(b);
    g.pend_lib = lib;
    g.pend_has_row = row != nullptr;
    g.pend_row = row ? *row : LibraryRow{};
    g.pend_proj = EnumProjects(-1, nullptr, 0);
    const uint64_t gen = ++g_batch_gen;
    for (const std::string& p : paths) {  // reached on the probe worker, then opened and read
        Task t;
        t.kind = TaskKind::Reach;
        t.path = p;
        t.id = gen;
        t.owner = PeakOwner::Batch;
        g_probe.Push(std::move(t), false);
    }
}

// Decision 13: the tree's single selected row, else the library root.
void UseAtTarget(const std::vector<MediaPick>& picks)
{
    const std::vector<LibraryRow>& rows = SelectedLibraryRows();
    LibraryRow row;
    const LibraryRow* rp = nullptr;
    if (rows.size() == 1) {
        row = rows.front();
        rp = &row;
    }
    std::string lib = SelectedLibraryUid();
    UsePicks(lib, rp, picks);
}

void OnMediaDrop(const std::string& lib, const LibraryRow& row, const void* data, int size)
{
    if (!data || size != static_cast<int>(sizeof(uint32_t))) return;
    uint32_t gen = 0;
    std::memcpy(&gen, data, sizeof gen);
    if (gen != g.drag_gen || g.drag_picks.empty()) return;  // a stale drag
    std::vector<MediaPick> picks = std::move(g.drag_picks);
    g.drag_picks.clear();
    UsePicks(lib, &row, picks);
}

void BeginDrag(const std::vector<MediaPick>& picks, const std::string& what)
{
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        // A new drag (none last frame) gets a new generation; the picks follow the source.
        if (g.drag_frame != ImGui::GetFrameCount() - 1 && g.drag_frame != ImGui::GetFrameCount()) ++g.drag_gen;
        g.drag_frame = ImGui::GetFrameCount();
        g.drag_picks = picks;
        ImGui::SetDragDropPayload(kPayload, &g.drag_gen, sizeof g.drag_gen);
        ImGui::TextUnformatted(what.c_str());
        ImGui::EndDragDropSource();
    }
}

// Decision 5: re-applied whenever another story replaced the hooks.
void ApplyHooks()
{
    const LibraryTreeHooks& h = GetLibraryTreeHooks();
    if (h.media_payload_type == kPayload && h.media_drop) return;
    LibraryTreeHooks n = h;
    n.media_payload_type = kPayload;
    n.media_drop = &OnMediaDrop;
    SetLibraryTreeHooks(n);
}

// ---- Drawing helpers (Latin-1 default font: glyphs outside it drawn as shapes, 13-7a B8) --------

void DrawStar(ImDrawList* dl, ImVec2 c, float r, bool on)
{
    ImVec2 pts[10];
    for (int i = 0; i < 10; ++i) {
        const float a = -1.5707963f + i * 0.62831853f;
        const float rr = (i % 2) ? r * 0.45f : r;
        pts[i] = ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr);
    }
    if (on) {
        for (int i = 0; i < 10; i += 2)
            dl->AddTriangleFilled(pts[(i + 9) % 10], pts[i], pts[i + 1], ui::kWarn);
        ImVec2 inner[5] = {pts[1], pts[3], pts[5], pts[7], pts[9]};
        dl->AddConvexPolyFilled(inner, 5, ui::kWarn);
    } else {
        dl->AddPolyline(pts, 10, ui::kFaint, ImDrawFlags_Closed, 1.0f);
    }
}

void DrawSpeaker(ImDrawList* dl, ImVec2 p, float s, ImU32 col, bool on)
{
    const float k = s / 16.0f;
    auto P = [&](float x, float y) { return ImVec2(p.x + x * k, p.y + y * k); };
    ImVec2 body[4] = {P(2.5f, 6.0f), P(5.0f, 6.0f), P(5.0f, 10.0f), P(2.5f, 10.0f)};
    dl->AddConvexPolyFilled(body, 4, col);
    ImVec2 cone[4] = {P(5.0f, 6.0f), P(8.5f, 3.0f), P(8.5f, 13.0f), P(5.0f, 10.0f)};
    dl->AddConvexPolyFilled(cone, 4, col);
    if (on) {
        dl->PathArcTo(P(8.5f, 8.0f), 3.0f * k, -0.9f, 0.9f, 8);
        dl->PathStroke(col, ImDrawFlags_None, 1.2f);
        dl->PathArcTo(P(8.5f, 8.0f), 5.5f * k, -0.9f, 0.9f, 10);
        dl->PathStroke(col, ImDrawFlags_None, 1.2f);
    } else {
        dl->AddLine(P(10.5f, 5.5f), P(14.0f, 10.5f), col, 1.2f);
        dl->AddLine(P(14.0f, 5.5f), P(10.5f, 10.5f), col, 1.2f);
    }
}

// The waveform: neutral peaks, framed variations with their number (accent when selected), a
// white playhead (file time, -1 = none).
void DrawMediaWaveform(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Analysis& an, const std::vector<int>& vsel,
                       double playhead)
{
    dl->AddRectFilled(p0, p1, ui::kBg, ui::kRadiusSm);
    dl->AddRect(p0, p1, ui::kStroke, ui::kRadiusSm);
    const float  w = p1.x - p0.x, h = p1.y - p0.y, mid = (p0.y + p1.y) * 0.5f;
    const double len = an.length;
    if (!(len > 0.0) || w < 4.0f) return;
    auto X = [&](double t) { return p0.x + static_cast<float>(t / len) * w; };
    for (size_t i = 0; i < an.frames.size(); ++i) {
        const SoundFrame& f = an.frames[i];
        const bool sel = std::find(vsel.begin(), vsel.end(), static_cast<int>(i)) != vsel.end();
        ImVec2 a(X(f.start), p0.y + 1.0f), b(X(f.start + f.length), p1.y - 1.0f);
        dl->AddRectFilled(a, b, sel ? ui::kAccentSoft : ui::kRaised);
        dl->AddRect(a, b, sel ? ui::kAccent : ui::kStrokeStrong);
        char num[16];
        std::snprintf(num, sizeof num, "%d", static_cast<int>(i) + 1);
        if (b.x - a.x > 12.0f) dl->AddText(ImVec2(a.x + 3.0f, a.y + 1.0f), sel ? ui::kAccent : ui::kMuted, num);
    }
    if (an.ready && an.peaks.rate > 0.0 && !an.peaks.max_abs.empty()) {
        const size_t n = an.peaks.max_abs.size();
        for (int x = 0; x < static_cast<int>(w); ++x) {
            const double t0 = len * x / w, t1 = len * (x + 1) / w;
            size_t       i0 = static_cast<size_t>(t0 * an.peaks.rate), i1 = static_cast<size_t>(t1 * an.peaks.rate);
            i1 = std::max(i1, i0 + 1);
            float m = 0.0f;
            for (size_t i = i0; i < i1 && i < n; ++i) m = std::max(m, an.peaks.max_abs[i]);
            const float amp = std::sqrt(std::min(1.0f, m)) * (h * 0.46f);
            const int   fi = FrameAt(an.frames, (t0 + t1) * 0.5);
            const bool  sel = fi >= 0 && std::find(vsel.begin(), vsel.end(), fi) != vsel.end();
            dl->AddLine(ImVec2(p0.x + x + 0.5f, mid - amp), ImVec2(p0.x + x + 0.5f, mid + amp + 1.0f),
                        sel ? ui::kAccent : ui::kMuted);
        }
    } else {
        const char* t = an.ready ? "No audio" : "Reading...";
        ImVec2      ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2((p0.x + p1.x - ts.x) * 0.5f, mid - ts.y * 0.5f), ui::kFaint, t);
    }
    if (playhead >= 0.0) dl->AddLine(ImVec2(X(playhead), p0.y), ImVec2(X(playhead), p1.y), ui::kWhite, 1.5f);
}

void PlayFile(const DbFile& f)
{
    if (IsMissing(f.path)) return;
    StartPreview(f.path, 0.0, -1.0, -1);
}

void PlayFrame(int i)
{
    if (!g.has_cur || i < 0 || i >= static_cast<int>(g.an.frames.size())) return;
    const SoundFrame& fr = g.an.frames[i];
    StartPreview(g.cur.path, fr.start, fr.start + fr.length, i);
}

// ---- Sections ------------------------------------------------------------------------------------

void DrawHeader()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::TextUnformatted("Media Explorer");
    ImGui::SameLine();
    const MediaDbInfo& info = g.info;
    std::string status;
    if (!MediaDbReady()) status = "Loading...";
    else {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%zu files \xC2\xB7 %zu database%s", info.files, info.databases.size(),
                      info.databases.size() == 1 ? "" : "s");
        status = buf;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::kMuted));
    ImGui::TextUnformatted(status.c_str());
    ImGui::PopStyleColor();
    // Right side: the auto-play toggle, then Reload.
    const float rw = ImGui::CalcTextSize("Reload").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float sz = ImGui::GetFrameHeight();
    const float right = ImGui::GetWindowContentRegionMax().x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), right - rw - sz - ImGui::GetStyle().ItemSpacing.x));
    ImVec2 sp = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##meauto", ImVec2(sz, sz))) {
        g.autoplay = !g.autoplay;
        SavePrefBool(kAutoPref, g.autoplay);
    }
    const bool hov = ImGui::IsItemHovered();
    if (g.autoplay || hov)
        dl->AddRectFilled(sp, ImVec2(sp.x + sz, sp.y + sz), g.autoplay ? ui::kAccentSoft : ui::kHover, ui::kRadiusSm);
    DrawSpeaker(dl, ImVec2(sp.x + (sz - 16.0f) * 0.5f, sp.y + (sz - 16.0f) * 0.5f), 16.0f,
                g.autoplay ? ui::kAccent : ui::kMuted, g.autoplay);
    if (hov) ImGui::SetTooltip("Auto-play on select: %s", g.autoplay ? "on" : "off");
    ImGui::SameLine();
    if (ui::SolidButton("Reload")) {
        MediaDbStartLoad(ResourcePath());
        g.probe.clear();
        g.info_ready = false;
        g.favs_loaded = false;  // favourites edited in REAPER's Media Explorer since are read again
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Read REAPER's media databases again");
}

void DrawSearch()
{
    const float cw = 120.0f;
    ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - cw - ImGui::GetStyle().ItemSpacing.x));
    if (ImGui::InputTextWithHint("##mesearch", "Search media databases", g.query, sizeof g.query)) {
        g.edited_at = ImGui::GetTime();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(cw);
    const int   ndb = static_cast<int>(g.info.databases.size());
    if (g.filter > ndb + 1) g.filter = 0;
    std::string preview = g.filter == 0 ? "All" : g.filter == ndb + 1 ? "Favourites" : g.info.databases[g.filter - 1].name;
    if (ImGui::BeginCombo("##medb", preview.c_str())) {
        for (int i = 0; i <= ndb + 1; ++i) {
            std::string label = i == 0 ? "All" : i == ndb + 1 ? "Favourites" : g.info.databases[i - 1].name;
            ImGui::PushID(i);
            if (ImGui::Selectable(label.c_str(), g.filter == i) && g.filter != i) {
                g.filter = i;
                g.edited_at = ImGui::GetTime();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}

void DrawResults(float h)
{
    if (!ImGui::BeginChild("##meresults", ImVec2(0.0f, h), ImGuiChildFlags_Border)) {
        ImGui::EndChild();
        return;
    }
    const int ndb = static_cast<int>(g.info.databases.size());
    if (g.results.empty()) {
        std::string hint;
        if (!MediaDbReady()) hint = "Loading the media databases...";
        else if (g.info.databases.empty()) hint = "No media database found: build one in REAPER's Media Explorer";
        else if (g.filter == ndb + 1) hint = "No favourite matches";
        else if (g.query[0] == 0) {
            char buf[128];
            std::snprintf(buf, sizeof buf, "Type to search %zu files in %zu databases", g.info.files, g.info.databases.size());
            hint = buf;
        } else hint = std::string("No file matches \"") + g.query + "\"";
        ui::SubText(hint.c_str());
        ImGui::EndChild();
        return;
    }
    ImDrawList*   dl = ImGui::GetWindowDrawList();
    const float   lh = ImGui::GetTextLineHeight();
    const float   rh = lh * 2.0f + 6.0f;
    const float   w = ImGui::GetContentRegionAvail().x;
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(g.results.size()) + (g.capped ? 1 : 0), rh);
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            if (i >= static_cast<int>(g.results.size())) {
                ui::SubText("1000+ files: type more words to narrow");
                continue;
            }
            const DbFile& f = *g.results[i];
            const bool    miss = IsMissing(f.path);
            const bool    sel = IsSelected(f.path);
            ImGui::PushID(i);
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (i % 2) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rh), ui::kRaised);
            const bool clicked = ImGui::Selectable("##r", sel, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(w, rh));
            const bool dbl = clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            const ImVec2 star_c(p.x + 9.0f, p.y + 3.0f + lh * 0.5f);
            const bool on_star = ImGui::GetIO().MousePos.x < p.x + 18.0f;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", miss ? "Not found on this PC"
                                             : (f.path + "\nclick to listen, drag onto the tree, double-click to use").c_str());
            if (clicked && on_star) {
                if (!dbl) ToggleFav(f.path);  // the double-click's second press would undo the first
            } else if (dbl && !miss) {
                UseAtTarget({WholePick(f)});
            } else if (clicked) {
                bool added = true;
                if (ImGui::GetIO().KeyCtrl) {
                    auto it = std::find_if(g.sel.begin(), g.sel.end(), [&](const DbFile& s) { return s.path == f.path; });
                    if (it != g.sel.end()) {
                        g.sel.erase(it);
                        added = false;  // un-selected: neither shown nor played
                    } else {
                        g.sel.push_back(f);
                    }
                } else {
                    g.sel.assign(1, f);
                }
                g.vsel.clear();
                if (added) {
                    SetCurrent(f);
                    if (miss) StopPreviewNow();
                    else if (g.autoplay) PlayFile(f);
                }
            }
            if (!miss) {
                std::vector<MediaPick> picks;
                if (IsSelected(f.path) && g.sel.size() > 1) {
                    for (const DbFile& s : g.sel)
                        if (!IsMissing(s.path)) picks.push_back(WholePick(s));
                } else {
                    picks.push_back(WholePick(f));
                }
                BeginDrag(picks, picks.size() > 1 ? std::to_string(picks.size()) + " files" : MediaFileName(f.path));
            }
            const ImU32 tc = miss ? ui::kFaint : ui::kText;
            DrawStar(dl, star_c, 5.5f, IsFav(f.path));
            const std::string fmt = FormatMediaFormat(f) + (f.length >= 0.0 ? "  " + FormatMediaLength(f.length) : "");
            const float fw = ImGui::CalcTextSize(fmt.c_str()).x;
            const float name_x = p.x + 20.0f, right = p.x + w - 4.0f;
            const std::string name = MediaFileName(f.path);
            ui::EllipsisText(dl, ImVec2(name_x, p.y + 3.0f), right - fw - 8.0f, tc, name);
            ui::EllipsisText(dl, ImVec2(name_x + 0.6f, p.y + 3.0f), right - fw - 8.0f, tc, name);  // bold
            dl->AddText(ImVec2(right - fw, p.y + 3.0f), ui::kMuted, fmt.c_str());
            std::string l2 = f.description;
            if (!f.library.empty()) l2 += (l2.empty() ? "(" : " (") + f.library + ")";
            if (miss) l2 = "Not found on this PC";
            else if (Probe(f.path) == 0) l2 = "checking...";
            ui::EllipsisText(dl, ImVec2(name_x, p.y + 3.0f + lh), right, ui::kFaint, l2);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void DrawPreview(float wave_h)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!g.has_cur) {
        ui::SubText("Click a result to see and hear it");
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        DrawMediaWaveform(dl, p, ImVec2(p.x + w, p.y + wave_h), Analysis{}, {}, -1.0);
        ImGui::Dummy(ImVec2(w, wave_h));
        return;
    }
    const bool miss = IsMissing(g.cur.path);
    char head[64];
    if (miss) std::snprintf(head, sizeof head, "Missing");
    else if (Probe(g.cur.path) == 0) std::snprintf(head, sizeof head, "Checking...");
    else if (!g.an.ready) std::snprintf(head, sizeof head, "Reading...");
    else if (g_pv.opening && g_pv.open_path == g.cur.path) std::snprintf(head, sizeof head, "Opening...");
    else std::snprintf(head, sizeof head, "%zu variation%s detected", g.an.frames.size(), g.an.frames.size() == 1 ? "" : "s");
    ImGui::TextUnformatted(head);
    ImGui::SameLine();
    const std::string len = FormatMediaLength(g.an.length);
    const float lw = ImGui::CalcTextSize(len.c_str()).x;
    const float gear = ImGui::GetFrameHeight();
    ImVec2 np = ImGui::GetCursorScreenPos();
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    ui::EllipsisText(dl, np, right - lw - gear - 12.0f, ui::kMuted, MediaFileName(g.cur.path));
    dl->AddText(ImVec2(right - lw - gear - 6.0f, np.y), ui::kMuted, len.c_str());
    ImGui::SetCursorScreenPos(ImVec2(right - gear, np.y - ImGui::GetStyle().FramePadding.y));
    ImGui::InvisibleButton("##megear", ImVec2(gear, gear));
    {
        // The gear: a ring with 6 teeth (drawn, the default font has no glyph for it).
        ImVec2 c(right - gear * 0.5f, np.y - ImGui::GetStyle().FramePadding.y + gear * 0.5f);
        const ImU32 col = ImGui::IsItemHovered() ? ui::kText : ui::kMuted;
        for (int k = 0; k < 6; ++k) {
            const float a = k * 1.0471976f;
            dl->AddLine(ImVec2(c.x + std::cos(a) * 3.5f, c.y + std::sin(a) * 3.5f),
                        ImVec2(c.x + std::cos(a) * 6.5f, c.y + std::sin(a) * 6.5f), col, 2.0f);
        }
        dl->AddCircle(c, 4.0f, col, 12, 1.5f);
    }
    if (ImGui::IsItemHovered()) {
        const SilenceParams sp{};
        ImGui::SetTooltip("Detection defaults for new sounds: split on silence, threshold %.0f dB, min gap %.0f ms, "
                          "pre-roll %.0f ms, tail %.0f ms (each sound can then be pre-edited in its panel)",
                          sp.threshold_db, sp.min_gap_ms, sp.preroll_ms, sp.tail_ms);
    }
    ImVec2      p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const double ph = (g_pv.playing && g_pv.path == g.cur.path) ? PreviewFileTime() : -1.0;
    DrawMediaWaveform(dl, p, ImVec2(p.x + w, p.y + wave_h), g.an, g.vsel, ph);
    ImGui::InvisibleButton("##mewave", ImVec2(std::max(1.0f, w), wave_h));
    if (miss) {
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Not found on this PC");
        return;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Click a variation to play it, Ctrl-click to select several, click between them to play from "
                          "there, drag onto the tree");
    if (ImGui::IsItemActivated() && g.an.length > 0.0) {
        const double t = std::clamp((ImGui::GetIO().MousePos.x - p.x) / w, 0.0f, 1.0f) * g.an.length;
        const int    fi = g.an.ready ? FrameAt(g.an.frames, t) : -1;
        if (fi >= 0) {
            if (ImGui::GetIO().KeyCtrl) {
                auto it = std::find(g.vsel.begin(), g.vsel.end(), fi);
                if (it != g.vsel.end()) g.vsel.erase(it);
                else g.vsel.push_back(fi);
            } else {
                g.vsel.assign(1, fi);
            }
            PlayFrame(fi);
        } else {
            StartPreview(g.cur.path, t, -1.0, -1);
        }
    }
    if (!g.vsel.empty() && g.an.ready) {
        std::vector<MediaPick> picks = SlicePicks(g.vsel);
        BeginDrag(picks, picks.size() > 1 ? std::to_string(picks.size()) + " variations"
                                          : MediaFileStem(g.cur.path) + " " + std::to_string(picks.front().index + 1));
    }
}

void DrawFooter()
{
    if (g.pending) {  // a use waiting for its files' variations
        const std::string msg = MediaReadingLabel(MediaBatchLeft(g.batch));
        const float cw = ImGui::CalcTextSize("Cancel").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImVec2 p = ImGui::GetCursorScreenPos();
        ui::EllipsisText(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + ImGui::GetStyle().FramePadding.y),
                         p.x + ImGui::GetContentRegionAvail().x - cw - 8.0f, ui::kMuted, msg);
        ImGui::SetCursorScreenPos(ImVec2(p.x + ImGui::GetContentRegionAvail().x - cw, p.y));
        if (ui::SolidButton("Cancel")) CancelPending();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop reading: no sound is added");
        return;
    }
    bool                   variations = false;
    std::vector<MediaPick> picks = UsedPicks(&variations);
    if (picks.empty()) return;
    const std::vector<LibraryRow>& rows = SelectedLibraryRows();
    const LibraryRow*              rp = rows.size() == 1 ? &rows.front() : nullptr;
    const std::string              lib = SelectedLibraryUid();
    std::string                    label;
    const SoundStore&              store = GetSoundStore(nullptr);
    if (lib.empty() || !FindLibrary(store, lib)) {
        ui::SubText("Create a library first");
        return;
    }
    if (!MediaUseLabel(store, lib, rp, static_cast<int>(picks.size()), variations, &label)) {
        ui::SubText("Pick a value row of this Switch first");
        return;
    }
    if (ui::SolidButton(label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) UseAtTarget(picks);
}

void HandleKeys()
{
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) return;
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        StopPreviewNow();
        return;
    }
    if (io.WantTextInput) return;
    if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        if (g_pv.playing || g_pv.opening) StopPreviewNow();
        else if (g.has_cur && g.vsel.size() == 1) PlayFrame(g.vsel.front());
        else if (g.has_cur) PlayFile(g.cur);
    }
}

}  // namespace

// ---- Public ----------------------------------------------------------------------------------------

void InitLibraryMedia(void* (*get_func)(const char* name))
{
    if (!get_func) return;
    g_quitting = false;
    f_play = reinterpret_cast<PlayPreviewFn>(get_func("PlayPreview"));
    f_stop = reinterpret_cast<StopPreviewFn>(get_func("StopPreview"));
    f_create = reinterpret_cast<CreateFromFileFn>(get_func("PCM_Source_CreateFromFile"));
}

void StopMediaPreview() { StopPreviewNow(); }

void QuitLibraryMedia()
{
    StopPreviewNow();
    CancelPending();
    // Both workers stop (a read in progress sees g_quitting) and are joined; untaken sources deleted.
    g_quitting = true;
    g_probe.Stop();
    g_reader.Stop();
    g.probe.clear();
    MediaDbStop();
    if (g_pv.cs_ready) {
        DeleteCriticalSection(&g_pv.reg.cs);
        g_pv.cs_ready = false;
    }
    g.results.clear();
    g.index.reset();
    g.drag_picks.clear();
}

void DrawMediaExplorerPanel()
{
    if (!g.prefs_loaded) {
        g.prefs_loaded = true;
        g.autoplay = LoadPrefBool(kAutoPref, true);
    }
    if (!g.load_started) {  // decision 6: the first draw starts the load (13-5's worker)
        g.load_started = true;
        MediaDbStartLoad(ResourcePath());
    }
    LoadFavs();
    ApplyHooks();
    // The panel was closed (not drawn) since the last frame: a waiting use is dropped, not made later.
    const int frame = ImGui::GetFrameCount();
    if (g.last_frame >= 0 && frame - g.last_frame > 1) {
        if (g.pending) {
            CancelPending();
            ShowLibraryToast("Use cancelled: the Media Explorer was closed", false);
        }
        if (g_pv.opening) {  // a slow open does not start playing minutes later
            g_pv.opening = false;
            ++g_open_gen;
        }
    }
    g.last_frame = frame;
    PollWorkers();
    // The published index changed (a load ended): new info, search again.
    if (MediaDbReady()) {
        std::shared_ptr<const MediaDbIndex> idx = MediaDbGet();
        if (!g.info_ready || idx != g.index) {
            g.info = MediaDbGetInfo();
            g.info_ready = true;
            g.dirty = true;
            g.probe.clear();
        }
    }
    if (g.edited_at >= 0.0 && ImGui::GetTime() - g.edited_at >= kDebounce) {
        g.edited_at = -1.0;
        g.dirty = true;
    }
    if (g.dirty && MediaDbReady()) RunSearch();

    ImGui::PushID("##media");
    DrawHeader();
    DrawSearch();
    const float wave_h = 72.0f;
    const float foot_h = ImGui::GetFrameHeightWithSpacing();
    const float prev_h = ImGui::GetTextLineHeightWithSpacing() + wave_h + ImGui::GetStyle().ItemSpacing.y * 2.0f;
    const float list_h = std::max(ImGui::GetTextLineHeight() * 4.0f, ImGui::GetContentRegionAvail().y - prev_h - foot_h);
    DrawResults(list_h);
    DrawPreview(wave_h);
    DrawFooter();
    HandleKeys();
    // A preview that reached its end is released even when the waveform is another file's.
    if (g_pv.playing) PreviewFileTime();
    ImGui::PopID();
}

}  // namespace rav

#endif  // _WIN32
