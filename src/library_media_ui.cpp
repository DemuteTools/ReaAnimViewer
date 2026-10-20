// SPDX-License-Identifier: MIT
//
// See library_media_ui.h. Spec 13-7c (DECISIONS section 6, mockup renderMediaExplorer / useFiles).
//
// Threads: the database load is media_db's worker (MediaDbStartLoad). This file has two more:
// the row checker (are the files of the rows on screen on this PC: one batch at a time, each path
// once per session, MediaExistsCache) and the probe worker (can the previewed / used file be
// reached: plain std::filesystem, exists and size, no REAPER call). The peaks are read by the one
// peak reader of sound_peaks (13-55: REAPER's .reapeaks file, on its own thread), asked only once
// the probe answered that the file is reachable, so an offline network share only leaves the
// preview "opening..." or a use "Reading N files...". The list itself comes straight from the index:
// a row is drawn at once and only dims once its file is known missing; the row checks never delay a
// preview or a use (their own worker). Every REAPER call (PCM_Source_CreateFromFile, deleting a
// source) stays on the UI thread. Results come back through a locked queue the draw polls; both
// workers are joined by QuitLibraryMedia. No folder is ever walked here.

#include "library_media_ui.h"
#include "watchdog.h"  // freeze audit: breadcrumbs and scopes

#ifdef _WIN32

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
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
#include <unordered_set>
#include <vector>

#include <imgui.h>

#include "console_log.h"
#include "library_media_plan.h"
#include "library_node_ui.h"
#include "library_tree_ui.h"
#include "library_view_ui.h"
#include "media_db.h"
#include "reaper_api.h"
#include "shortcuts.h"
#include "sound_audition.h"
#include "sound_audition_plan.h"  // ReachCache
#include "sound_entries.h"
#include "sound_peaks.h"
#include "sound_project.h"
#include "sound_record.h"
#include "ui_flow.h"
#include "library_icons.h"
#include "ui_theme.h"
#include "wave_draw_ui.h"  // 13-71: the one waveform drawer

namespace rav {

namespace {

using PlayPreviewFn = int (*)(preview_register_t* preview);
using StopPreviewFn = int (*)(preview_register_t* preview);
using PlayPreviewExFn = int (*)(preview_register_t* preview, int bufflags, double measure_align);
using CreateFromFileFn = PCM_source* (*)(const char* filename);

PlayPreviewFn    f_play = nullptr;
PlayPreviewExFn  f_play_ex = nullptr;  // freeze fix: buffered, so REAPER's audio thread never reads the file
StopPreviewFn    f_stop = nullptr;
CreateFromFileFn f_create = nullptr;

const char* const kPayload = "RAV_MEDIA";
const char* const kAutoPref = "lib_media_autoplay";
constexpr size_t  kMaxResults = 1000;
constexpr double  kDebounce = 0.2;

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
        RAV_AUDIO_SCOPE("Media Explorer preview");  // freeze audit
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
                RAV_AUDIO_ARG(inner_->GetFileName());  // freeze audit
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

// Exists: the rows' missing checks (a batch of paths). Reach: the file is there and answers a short
// read (then the UI thread opens it, or asks its peaks of sound_peaks).
enum class TaskKind { Exists, Reach };
enum class PeakOwner { Open, Preview, Batch, Audition };  // Open: the preview's playback; Audition: sound_audition's files (Reach only)

struct Task {
    TaskKind    kind = TaskKind::Exists;
    std::string path;
    uint64_t    id = 0;  // its owner's generation
    PeakOwner   owner = PeakOwner::Preview;
    std::vector<std::string> paths;  // Exists: the batch
};

struct TaskResult {
    TaskKind    kind = TaskKind::Exists;
    std::string path;
    uint64_t    id = 0;
    PeakOwner   owner = PeakOwner::Preview;
    bool        ok = false;  // the file is there
    long long   size = -1;   // Reach: its size when it is there
    std::vector<std::string> paths;  // Exists: the batch, and whether each file is there
    std::vector<char>        found;
};

// A newer request makes an older one stale: its answer is ignored.
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
    case PeakOwner::Audition: return false;  // a cheap answer, cached; its asker polls it
    default: return t.id != g_batch_gen;
    }
}

// One thread running tasks in order (front pushes first); results wait in `out_` for the draw.
class DiskWorker {
public:
    void Push(Task t, bool front)
    {
        if (g_quitting) return;
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
    // Unload: the queued tasks are dropped, the running one stops (g_quitting), the thread joined.
    void Stop()
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
            q_.clear();
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
        Take();
        std::lock_guard<std::mutex> lk(m_);
        stop_ = false;
    }

private:
    void Run()
    {
        const int slot = RAV_WORKER_SLOT("Media Explorer disk", 0);  // freeze audit
        (void)slot;
        for (;;) {
            RAV_WORKER_IDLE(slot);
            Task t;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                if (stop_) return;
                t = std::move(q_.front());
                q_.pop_front();
            }
            if (TaskStale(t)) continue;
            RAV_WORKER_BUSY(slot, "checking", t.path);
            TaskResult r;
            r.kind = t.kind;
            r.path = t.path;
            r.id = t.id;
            r.owner = t.owner;
            // Nothing may escape this thread (a throw would end REAPER): a failed task still answers,
            // so a waiting use never waits forever.
            try {
                if (t.kind == TaskKind::Exists) {
                    r.paths = std::move(t.paths);
                    r.found.assign(r.paths.size(), 1);  // unchecked at unload: not reported missing
                    for (size_t i = 0; i < r.paths.size() && !g_quitting; ++i) {
                        std::error_code ec;
                        r.found[i] = std::filesystem::exists(std::filesystem::u8path(r.paths[i]), ec) ? 1 : 0;
                    }
                } else {
                    std::error_code ec;
                    const std::filesystem::path fp = std::filesystem::u8path(t.path);
                    r.ok = std::filesystem::exists(fp, ec);
                    if (r.ok && t.kind == TaskKind::Reach) {  // its size answers too: the share is awake
                        const std::uintmax_t sz = std::filesystem::file_size(fp, ec);
                        r.ok = sz != static_cast<std::uintmax_t>(-1) && !ec;
                        if (r.ok) r.size = static_cast<long long>(sz);
                    }
                }
            } catch (...) {
                r.ok = false;
            }
            std::lock_guard<std::mutex> lk(m_);
            out_.push_back(std::move(r));
        }
    }

    std::thread             th_;
    std::mutex              m_;
    std::condition_variable cv_;
    std::deque<Task>        q_;
    std::vector<TaskResult> out_;
    bool                    stop_ = false;
};

DiskWorker g_rows;    // Exists: the rows' checks, never ahead of a preview or a use
DiskWorker g_probe;   // Reach (quick, but may stall on an offline share)

// Freeze fix (audit #1): the files the probe reached lately, for sound_audition (which asks before it opens).
ReachCache            g_reach;
std::set<std::string> g_reach_inflight;  // asked and not answered yet (main thread): one task per path at a time
double     ReachNow()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

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
    double             last_cur = 0.0;  // the last position read (kept when the preview's lock is busy)
    // A start waiting for its source (opened on the probe worker).
    bool               opening = false;
    std::string        open_path;
    double             open_from = 0.0, open_to = -1.0;
    int                open_frame = -1;
};
Preview g_pv;

// Freeze fix (audit #1): a buffered preview (REAPER reads the source on its own buffering thread, so a file that
// stalls -- a cloud drive that is not cached, a share that dropped -- never holds REAPER's audio thread or the
// preview's lock); the plain PlayPreview only when this REAPER has no PlayPreviewEx.
int StartReaperPreview(preview_register_t* reg)
{
    if (f_play_ex && f_play_ex(reg, 1 /*buffer the source*/, -1.0 /*now*/) != 0) return 1;
    return f_play ? f_play(reg) : 0;  // a source REAPER cannot buffer (no Duplicate): played as before
}

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
    g_pv.last_cur = cur;
    g_pv.playing = StartReaperPreview(&g_pv.reg) != 0;
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
    // Never wait on the preview's lock (freeze audit #1): a busy one keeps the last position.
    if (TryEnterCriticalSection(&g_pv.reg.cs)) {
        g_pv.last_cur = g_pv.reg.curpos;
        LeaveCriticalSection(&g_pv.reg.cs);
    }
    const double cur = g_pv.last_cur;
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
    bool                    reached = false; // the probe found it: its peaks are asked of sound_peaks
    double                  length = -1.0;
    PeakData                peaks;
    std::vector<SoundFrame> frames;
    std::chrono::steady_clock::time_point shown_at;  // 13-57 (debuglog): when it was clicked
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
    MediaExistsCache                    exists;     // per path, for the session (the rows on screen)
    std::vector<DbFile>                 sel;        // selected files (copies; set through SelSet / SelToggle)
    std::unordered_set<std::string>     sel_keys;   // 13-69: their paths (IsSelected in O(1) after Ctrl+A)
    int                                 anchor_row = -1;  // 13-69: results index a Shift+click ranges from
    std::string                         searched;   // 13-69: the query of the results (the matched-field tooltip)
    bool                                drag_alt = false;  // 13-69: Alt held during the drag (the drop's frame included)
    std::string                         hover_lib;  // 13-69: the drop target under a media drag (media_hover)
    LibraryRow                          hover_row;
    int                                 hover_frame = -10;
    bool                                pend_as_random = false;  // 13-69: the waiting use makes one new Random
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
    std::vector<std::string>            pend_reached;  // its files reached: peaks asked of sound_peaks
    ReaProject*                         pend_proj = nullptr;  // the project the use was made in
    int                                 last_frame = -1;  // the last frame the panel was drawn
    int                                 scroll_to = -1;   // a result row to bring into view (arrow keys)
    int                                 cur_row = -1;     // results index of the keyboard row (checked before use)
    bool                                takes_arrows = false;  // the panel is focused with results: Up/Down are ours
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

// Known missing (a file not checked yet is not: it is drawn and used as found). Never touches
// the disk: the rows on screen are checked by CheckVisibleRows, the previewed / used files when
// reached.
bool IsMissing(const std::string& path) { return g.exists.Missing(path); }

// Decision 8: after the list is drawn, the visible rows whose files are not known yet are checked
// in one batch on the row checker (one batch out at a time, each path once for the session).
void CheckVisibleRows()
{
    uint64_t gen = 0;
    std::vector<std::string> batch = g.exists.EndFrame(64, &gen);
    if (batch.empty()) return;
    Task t;
    t.kind = TaskKind::Exists;
    t.id = gen;
    t.paths = std::move(batch);
    g_rows.Push(std::move(t), false);
}

void RunSearch()
{
    g.dirty = false;
    g.index = MediaDbGet();
    g.results.clear();
    g.capped = false;
    g.anchor_row = -1;  // 13-69: the rows moved
    const std::string q = g.query;
    g.searched = q;
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
    } else {
        std::string db = (g.filter >= 1 && g.filter <= ndb) ? g.info.databases[g.filter - 1].name : std::string();
        // An empty search lists every file of the filter (the clipper draws only the visible rows).
        if (blank) g.results = MediaListAll(g.index->Files(), db);
        else g.results = g.index->Search(q, db, kMaxResults + 1);
    }
    if (!blank && g.results.size() > kMaxResults) {
        g.results.resize(kMaxResults);
        g.capped = true;
    }
}

bool IsSelected(const std::string& path)
{
    return g.sel_keys.count(path) != 0;
}

// 13-69: the selection, its keys kept with it.
void SelSet(std::vector<DbFile> files)
{
    g.sel = std::move(files);
    g.sel_keys.clear();
    for (const DbFile& f : g.sel) g.sel_keys.insert(f.path);
}

// Ctrl+click: false when the file was selected (now un-selected).
bool SelToggle(const DbFile& f)
{
    if (g.sel_keys.erase(f.path)) {
        g.sel.erase(std::remove_if(g.sel.begin(), g.sel.end(), [&](const DbFile& s) { return s.path == f.path; }),
                    g.sel.end());
        return false;
    }
    g.sel.push_back(f);
    g.sel_keys.insert(f.path);
    return true;
}

// 13-69: Shift+click (Ctrl+Shift adds to the selection) and Ctrl+A: results rows, at most
// kMaxResults files (a blank search lists the whole database).
void SelRows(const std::vector<size_t>& rows, bool add)
{
    std::vector<DbFile> files = add ? g.sel : std::vector<DbFile>{};
    std::unordered_set<std::string> keys = add ? g.sel_keys : std::unordered_set<std::string>{};
    for (size_t i : rows) {
        if (i >= g.results.size()) continue;
        const DbFile& f = *g.results[i];
        if (keys.insert(f.path).second) files.push_back(f);
    }
    SelSet(std::move(files));
}

// The previewed file's peaks (read, or failed to open / read).
void PreviewPeaks(bool ok, PeakData&& peaks, double length)
{
    g.an.peaks = std::move(peaks);
    if (length > 0.0) g.an.length = length;
    g.an.frames = ok ? SplitOnSilence(g.an.peaks, SilenceParams{}, g.an.length) : std::vector<SoundFrame>{};
    g.an.failed = !ok;
    g.an.ready = true;
#ifdef RAV_ENABLE_CONSOLE_LOG
    LogInfo("media: %s waveform shown %.0f ms after the click", g.an.path.c_str(),
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g.an.shown_at).count());
#endif
    // A double-click previews and uses the same file: the use takes this read, not a second one.
    if (g.pending) MediaBatchSetRead(g.batch, g.an.path, ok, g.an.frames, length);
}

// Shows a file in the preview (reached on the probe, then its peaks asked of sound_peaks).
void SetCurrent(const DbFile& f)
{
    if (g.has_cur && g.cur.path == f.path) return;
    g.cur = f;
    g.has_cur = true;
    g.vsel.clear();
    g.an = Analysis{};
    g.an.path = f.path;
    g.an.length = f.length;
    g.an.shown_at = std::chrono::steady_clock::now();
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

// 13-55: the peaks of the files reached, asked of the one peak reader (sound_peaks) each frame until
// it answers (a map lookup per file: the reading is on its thread). A file nobody asks for any more
// (another file previewed, the use cancelled) is dropped from its wait.
void AskPeaks()
{
    if (g.has_cur && g.an.reached && !g.an.ready) {
        PeakData        pk;
        double          len = 0.0;
        const PeakState st = RequestPeaks(g.an.path, &pk, &len);
        if (st != PeakState::Building) PreviewPeaks(st == PeakState::Ready && len > 0.0, std::move(pk), len);
    }
    if (!g.pending) return;
    for (size_t i = 0; i < g.pend_reached.size();) {
        const std::string path = g.pend_reached[i];
        PeakData          pk;
        double            len = 0.0;
        const PeakState   st = RequestPeaks(path, &pk, &len);
        if (st == PeakState::Building) {
            ++i;
            continue;
        }
        const bool              ok = st == PeakState::Ready && len > 0.0;
        std::vector<SoundFrame> frames;
        if (ok) frames = SplitOnSilence(pk, SilenceParams{}, len);
        MediaBatchSetRead(g.batch, path, ok, frames, len);
        g.pend_reached.erase(g.pend_reached.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void CommitPending();

// The workers' results (rows found or missing, the preview's source, the files reached) and the
// peaks read.
void PollWorkers()
{
    for (TaskResult& r : g_rows.Take()) {
        g.exists.Answer(r.id, r.paths, r.found);
        if (g.has_cur && !g.an.ready && IsMissing(g.cur.path)) {
            ++g_preview_gen;  // found missing while its peaks wait: no audio
            g.an.ready = true;
        }
    }
    for (TaskResult& r : g_probe.Take()) {
        g.exists.Set(r.path, r.ok);
        g_reach.Set(r.path, r.ok, ReachNow(), r.size);
        g_reach_inflight.erase(r.path);
        if (r.owner == PeakOwner::Audition) continue;  // only the cache: sound_audition reads it
        if (r.owner == PeakOwner::Open) {
            if (!g_pv.opening || r.id != g_open_gen) continue;
            PCM_source* src = r.ok && f_create ? f_create(r.path.c_str()) : nullptr;
            if (src) FinishPreview(src);
            else g_pv.opening = false;  // missing, or it did not open
        } else if (r.owner == PeakOwner::Preview) {
            if (r.id != g_preview_gen || r.path != g.an.path) continue;
#ifdef RAV_ENABLE_CONSOLE_LOG
            // 13-57 (debuglog builds): the first stage of a preview's "Reading...": the file reached.
            LogInfo("media: %s %s in %.0f ms", r.path.c_str(), r.ok ? "reached" : "missing",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g.an.shown_at).count());
#endif
            if (r.ok) g.an.reached = true;
            else g.an.ready = true;  // missing: no audio
        } else if (g.pending && r.id == g_batch_gen) {
            if (r.ok) g.pend_reached.push_back(r.path);
            else MediaBatchSetMissing(g.batch, r.path);  // dropped from the use, named in the toast
        }
    }
    AskPeaks();
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
                const std::string& suffix, bool as_random)
{
    if (picks.empty()) return;
    std::string toast, why, rnd;
    std::vector<std::string> created;
    const char* desc = MediaUseAddsInside(GetSoundStore(nullptr), lib, row) ? "RAV: Add sounds"
                                                                             : "RAV: Set sound from Media Explorer";
    bool ok = ModifySoundStore(nullptr, desc, [&](SoundStore& s) {
        // 13-67c: the new Sounds' zones laid out on their files' peaks (read by the use already), as Edit shows them.
        auto peaks = [](const std::string& path, PeakData* out) {
            return !path.empty() && RequestPeaks(path, out) == PeakState::Ready;
        };
        return UseMediaAt(s, lib, row, picks, &created, &toast, &why, as_random, &rnd, peaks);
    });
    if (ok) {
        ShowLibraryToast(toast + suffix, true, desc);
        g.vsel.clear();
        // 13-69: the new Random is selected in the tree.
        if (!rnd.empty())
            if (const SoundLibrary* l = FindLibrary(GetSoundStore(nullptr), lib)) SelectLibraryRow(lib, RowOfNode(*l, rnd));
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
    g.pend_reached.clear();
    ++g_batch_gen;
}

void CommitPending()
{
    if (!g.pending) return;
    MediaBatch        b = std::move(g.batch);
    const std::string lib = g.pend_lib;
    const LibraryRow  row = g.pend_row;
    const bool        has_row = g.pend_has_row;
    const bool        as_random = g.pend_as_random;
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
    ApplyPicks(lib, has_row ? &row : nullptr, used, suffix, as_random);
}

// A use: every whole file whose variations are not known yet is read first ("Reading N
// files..."), then all the Sounds are made at once. A new use replaces a waiting one.
void UsePicks(const std::string& lib, const LibraryRow* row, const std::vector<MediaPick>& picks,
              bool as_random = false)
{
    CancelPending();
    if (picks.empty()) return;
    MediaBatch b = StartMediaBatch(picks);
    const std::vector<std::string> paths = MediaBatchPathsToRead(b);
    if (paths.empty()) {
        ApplyPicks(lib, row, b.picks, std::string(), as_random);
        return;
    }
    g.pending = true;
    g.pend_as_random = as_random;
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
void UseAtTarget(const std::vector<MediaPick>& picks, bool as_random = false)
{
    const std::vector<LibraryRow>& rows = SelectedLibraryRows();
    LibraryRow row;
    const LibraryRow* rp = nullptr;
    if (rows.size() == 1) {
        row = rows.front();
        rp = &row;
    }
    std::string lib = SelectedLibraryUid();
    UsePicks(lib, rp, picks, as_random);
}

void OnMediaDrop(const std::string& lib, const LibraryRow& row, const void* data, int size)
{
    if (!data || size != static_cast<int>(sizeof(uint32_t))) return;
    uint32_t gen = 0;
    std::memcpy(&gen, data, sizeof gen);
    if (gen != g.drag_gen || g.drag_picks.empty()) return;  // a stale drag
    std::vector<MediaPick> picks = std::move(g.drag_picks);
    g.drag_picks.clear();
    UsePicks(lib, &row, picks, g.drag_alt);  // 13-69: Alt held at the drop = one new Random
}

// 13-69: the drop target under the drag (LibraryTreeHooks::media_hover), for the drag's tooltip.
void OnMediaHover(const std::string& lib, const LibraryRow& row)
{
    g.hover_lib = lib;
    g.hover_row = row;
    g.hover_frame = ImGui::GetFrameCount();
}

void BeginDrag(const std::vector<MediaPick>& picks, const std::string& what)
{
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        // A new drag (none last frame) gets a new generation; the picks follow the source.
        if (g.drag_frame != ImGui::GetFrameCount() - 1 && g.drag_frame != ImGui::GetFrameCount()) ++g.drag_gen;
        g.drag_frame = ImGui::GetFrameCount();
        g.drag_picks = picks;
        g.drag_alt = ImGui::GetIO().KeyAlt;
        ImGui::SetDragDropPayload(kPayload, &g.drag_gen, sizeof g.drag_gen);
        // 13-69: what the drop makes on the target under the mouse (set by it last frame or this one).
        std::string hint;
        if (g.hover_frame >= ImGui::GetFrameCount() - 1) {
            bool variations = false;
            for (const MediaPick& p : picks) variations = variations || p.slice;
            hint = MediaDropHint(GetSoundStore(nullptr), g.hover_lib, &g.hover_row, static_cast<int>(picks.size()),
                                 variations, g.drag_alt);
        }
        ImGui::TextUnformatted(hint.empty() ? what.c_str() : hint.c_str());
        if (hint.empty())
            ui::ToneText("Drop on a container, a value row or empty space in the tree,\n"
                         "or on a Sound's waveform to replace its audio");
        if (picks.size() >= 2 && !g.drag_alt) {
            bool variations = false;
            for (const MediaPick& p : picks) variations = variations || p.slice;
            ui::ToneText(variations ? "Hold Alt at the drop: one sound per variation, in a new Random"
                                    : "Hold Alt at the drop: one new Random of them");
        }
        ImGui::EndDragDropSource();
    }
}

// The Sound panel's drop (13-7b): the dragged picks of the first file become its new source; the
// dragged variations of that file (or the file's detected ones) its frames. False on a stale drag.
bool DecodeMediaDrop(const void* data, int size, SoundSource* source, std::vector<SoundFrame>* frames)
{
    if (!data || size != static_cast<int>(sizeof(uint32_t)) || !source || !frames) return false;
    uint32_t gen = 0;
    std::memcpy(&gen, data, sizeof gen);
    if (gen != g.drag_gen || g.drag_picks.empty()) return false;
    const std::vector<MediaPick> picks = std::move(g.drag_picks);
    g.drag_picks.clear();
    const MediaPick& first = picks.front();
    *source = SoundSpecFor(first).sound.source;
    frames->clear();
    for (const MediaPick& p : picks) {
        if (p.path != first.path) continue;
        if (p.slice) {
            SoundFrame f = p.frame;
            f.hand = true;
            frames->push_back(f);
        } else {
            *frames = SoundSpecFor(p).sound.frames;
            break;
        }
    }
    return true;
}

// "Show in Media Explorer": searches the file's name in every database.
void ShowInMedia(const SoundSource& src)
{
    const std::string stem = MediaFileStem(src.asset.name.empty() ? src.asset.last_path : src.asset.name);
    SetLibraryMediaCollapsed(false);  // the Media column is shown to show the file
    std::snprintf(g.query, sizeof g.query, "%s", stem.c_str());
    g.filter = 0;
    g.dirty = true;
}

// Decision 5: the tree's media members are re-applied whenever another story replaced the hooks
// (called at init and before every draw of the Library view's columns that this panel draws,
// so a drop never depends on the column draw order). The Sound panel's hooks are set once.
void ApplyHooks()
{
    static bool node_set = false;
    if (!node_set) {
        node_set = true;
        NodePanelHooks nh;
        nh.media_payload_type = kPayload;
        nh.decode_media = &DecodeMediaDrop;
        nh.show_in_media = &ShowInMedia;
        SetNodePanelHooks(nh);
    }
    const LibraryTreeHooks& h = GetLibraryTreeHooks();
    if (h.media_payload_type == kPayload && h.media_drop && h.media_hover) return;
    LibraryTreeHooks n = h;
    n.media_payload_type = kPayload;
    n.media_drop = &OnMediaDrop;
    n.media_hover = &OnMediaHover;
    SetLibraryTreeHooks(n);
}

// ---- Drawing helpers (13-7c: the mockup's .fav star, .wave and .vbox) ---------------------------

// The favourite star (.fav): drawn, outlined in `col` when off, filled in warn when on.
void DrawStar(ImDrawList* dl, ImVec2 c, float r, bool on, ImU32 col)
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
        dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, 1.0f);
    }
}

// The waveform (the mockup's .wave, DECISIONS section 6): kBg, 1 px stroke edge, r5. The peaks are
// neutral (muted), accent inside the selected variations, past full scale in the clip colour.
// Each variation is a box (.vbox): inset 2 px top and bottom, 1 px stroke-strong edge r3, accent
// at ~9 % (~13 % under the mouse); a selected one at ~18 % with an accent edge and an inner
// accent-line edge. Its number in mono 9 at (3, 1), muted (selected: accent text). The playhead
// is a 1 px text-coloured line (file time, -1 = none). `hover`: the box under the mouse, -1 none.
void DrawMediaWaveform(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Analysis& an, const std::vector<int>& vsel,
                       double playhead, int hover = -1)
{
    dl->AddRectFilled(p0, p1, ui::kBg, ui::kRadiusSm);
    const float  w = p1.x - p0.x, mid = (p0.y + p1.y) * 0.5f;
    // 13-14 (C7.5) / 13-71: a lane per channel (L above R), each its share of the height, a 1 px
    // stroke-strong line between them (DrawWaveLanes).
    const double len = an.length;
    if (!(len > 0.0) || w < 4.0f) {
        dl->AddRect(p0, p1, ui::kStroke, ui::kRadiusSm);
        return;
    }
    dl->PushClipRect(p0, p1, true);
    auto X = [&](double t) { return p0.x + static_cast<float>(t / len) * w; };
    auto Sel = [&](int i) { return std::find(vsel.begin(), vsel.end(), i) != vsel.end(); };
    const float inset = ui::Dp(2.0f), r3 = ui::Dp(3.0f);
    for (size_t i = 0; i < an.frames.size(); ++i) {
        const SoundFrame& f = an.frames[i];
        const int         k = static_cast<int>(i);
        const int         a = Sel(k) ? 0x2E : (k == hover ? 0x21 : 0x17);  // 18 % / 13 % / 9 %
        dl->AddRectFilled(ImVec2(X(f.start), p0.y + inset), ImVec2(X(f.start + f.length), p1.y - inset),
                          ui::WithAlpha(ui::kAccent, a), r3);
    }
    if (an.ready && an.peaks.rate > 0.0 && !an.peaks.max_abs.empty()) {
        // 13-57: one column per pixel read at the panel's resolution from the samples (sharp, its true
        // shape: signed min / max), the read peaks meanwhile; the samples as a line for a file so short
        // a pixel is shorter than a sample.
        const int   ncols = std::max(1, static_cast<int>(w));
        WaveColumns wc;
        if (!ReadWaveColumns(an.path, 0.0, len, ncols, &wc)) ColumnsFromPeaks(an.peaks, 0.0, len, ncols, &wc);
        WaveDrawStyle st;
        st.pad = ui::Dp(WaveLanesFor(MonoMode::Off, wc.series).n > 1 ? 2.0f : 4.0f);
        st.line_w = ui::Dp(1.5f);
        st.split = ui::kStrokeStrong;
        DrawWaveLanes(dl, wc, MonoMode::Off, std::floor(p0.x), p0.y, p1.y, st, [&](double pos, double peak, bool, WaveInk* out) {
            const int fi = FrameAt(an.frames, pos / ncols * len);
            out[0].col = peak > 1.0 ? kLibMissing : (fi >= 0 && Sel(fi)) ? ui::kAccent : ui::kMuted;
            return 1;
        });
    } else {
        const char*  t = an.ready ? "No audio" : "Reading...";
        ImFont*      sf = ui::FontSmall();
        const ImVec2 ts = ui::TextSizeIn(sf, t);
        ui::AddTextIn(dl, sf, ImVec2((p0.x + p1.x - ts.x) * 0.5f, mid - ts.y * 0.5f), ui::kFaint, t);
    }
    ImFont* nf = ui::FontMonoMicro();
    for (size_t i = 0; i < an.frames.size(); ++i) {
        const SoundFrame& f = an.frames[i];
        const bool   sel = Sel(static_cast<int>(i));
        const float  xa = X(f.start), xb = X(f.start + f.length);
        const ImVec2 a(xa, p0.y + inset), b(xb, p1.y - inset);
        if (sel) {
            dl->AddRect(a, b, ui::kAccent, r3);
            dl->AddRect(ImVec2(a.x + 1.0f, a.y + 1.0f), ImVec2(b.x - 1.0f, b.y - 1.0f), ui::kAccentLine, r3);
        } else {
            dl->AddRect(a, b, ui::kStrokeStrong, r3);
        }
        char num[16];
        std::snprintf(num, sizeof num, "%d", static_cast<int>(i) + 1);
        const ImVec2 ns = ui::TextSizeIn(nf, num);
        if (xb - xa > ns.x + ui::Dp(6.0f))
            ui::AddTextIn(dl, nf, ImVec2(xa + ui::Dp(3.0f), a.y + ui::Dp(1.0f)), sel ? ui::kAccentText : ui::kMuted, num);
    }
    if (playhead >= 0.0) {
        const float px = std::floor(X(playhead)) + 0.5f;
        dl->AddLine(ImVec2(px, p0.y), ImVec2(px, p1.y), ui::kText, 1.0f);
    }
    dl->PopClipRect();
    dl->AddRect(p0, p1, ui::kStroke, ui::kRadiusSm);
}

// The variation under the mouse in a waveform at p0..p1 (-1: none, or the mouse is elsewhere).
int HoveredFrame(ImVec2 p0, ImVec2 p1, const Analysis& an)
{
    if (!an.ready || !(an.length > 0.0) || p1.x - p0.x < 4.0f) return -1;
    if (!ImGui::IsWindowHovered() || !ImGui::IsMouseHoveringRect(p0, p1)) return -1;
    const double t = std::clamp((ImGui::GetIO().MousePos.x - p0.x) / (p1.x - p0.x), 0.0f, 1.0f) * an.length;
    return FrameAt(an.frames, t);
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

// The databases' status, the card header's faint suffix ("405112 files - 4 databases").
std::string HeaderStatus()
{
    if (!MediaDbReady()) return "Loading...";
    char buf[96];
    std::snprintf(buf, sizeof buf, "%zu files \xC2\xB7 %zu database%s", g.info.files, g.info.databases.size(),
                  g.info.databases.size() == 1 ? "" : "s");
    return buf;
}

// How many of the header's buttons fit a card `inner_w` wide (the title keeps ~40 px):
// 3 = reload, collapse and speaker; 2 drops reload first; 1 keeps the speaker only.
int HeaderButtons(float inner_w)
{
    const float sz = ui::ControlH(), sp = ImGui::GetStyle().ItemSpacing.x;
    for (int n = 3; n > 1; --n)
        if (inner_w >= n * sz + (n - 1) * sp + ui::Dp(8.0f) + ui::Dp(40.0f)) return n;
    return 1;
}

float HeaderButtonsWidth(int n)
{
    return n * ui::ControlH() + (n - 1) * ImGui::GetStyle().ItemSpacing.x;
}

// The card header's right slot (.secx.me .secx-h): a quiet reload (RAV only), the collapse ">>"
// (.colbtn) and the auto-play speaker (.btn.sm.ico.spk: accent on accent-soft when on).
void DrawHeaderButtons(int n)
{
    if (n >= 3) {
        if (ui::IconOnlyButton("##mereload", "\xE2\x86\xBB", "Read REAPER's media databases again")) {
            MediaDbStartLoad(ResourcePath());
            g.exists.Clear();  // files moved since are checked again
            g.info_ready = false;
            g.favs_loaded = false;  // favourites edited in REAPER's Media Explorer since are read again
        }
        ImGui::SameLine();
    }
    if (n >= 2) {
        if (ui::IconOnlyButton("##mecollapse", "\xC2\xBB", "Collapse the Media Explorer")) SetLibraryMediaCollapsed(true);
        ImGui::SameLine();
    }
    ui::Icon spk;
    spk.fn = &DrawLibraryGlyph;
    spk.which = kGlyphSpeaker;
    spk.col = ui::kMuted;
    if (ui::IconOnlyButton("##meauto", spk, g.autoplay ? "Auto-play on select: on" : "Auto-play on select: off", 0.0f,
                           g.autoplay)) {
        g.autoplay = !g.autoplay;
        SavePrefBool(kAutoPref, g.autoplay);
    }
}

// The search row (.me-search): the dark field (input.txt) and the database filter (select) at its
// natural width beside it; under ~120 px of field the filter goes under it, both full width.
void DrawSearch()
{
    const int ndb = static_cast<int>(g.info.databases.size());
    if (g.filter > ndb + 1) g.filter = 0;
    const std::string preview =
        g.filter == 0 ? "All" : g.filter == ndb + 1 ? "Favourites" : g.info.databases[g.filter - 1].name;
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float cw = std::min(std::max(ui::Dp(70.0f), ui::TextWidth(preview.c_str()) + ui::Dp(28.0f)), ui::Dp(160.0f));
    const bool  stack = avail < ui::Dp(120.0f) + sp + cw;
    if (ui::TextField("##mesearch", "Search media databases", g.query, sizeof g.query, stack ? avail : avail - cw - sp))
        g.edited_at = ImGui::GetTime();
    if (!stack) ImGui::SameLine();
    if (ui::BeginComboStyled("##medb", preview.c_str(), stack ? avail : cw)) {
        for (int i = 0; i <= ndb + 1; ++i) {
            const std::string label = i == 0 ? "All" : i == ndb + 1 ? "Favourites" : g.info.databases[i - 1].name;
            ImGui::PushID(i);
            if (ImGui::Selectable(label.c_str(), g.filter == i) && g.filter != i) {
                g.filter = i;
                g.edited_at = ImGui::GetTime();
            }
            ImGui::PopID();
        }
        ui::EndComboStyled();
    }
}

// 13-69: the results' right-click menu: the footer's use, "Make a Random of the selection" (at the
// tree's selected row, as the footer's use), and select all.
bool g_open_ctx = false;

void DrawResultsMenu()
{
    if (g_open_ctx) {
        g_open_ctx = false;
        ImGui::OpenPopup("##mectx");
    }
    if (!ui::BeginMenuPopup("##mectx")) return;
    std::vector<MediaPick> picks;
    for (const DbFile& f : g.sel)
        if (!IsMissing(f.path)) picks.push_back(WholePick(f));
    const std::vector<LibraryRow>& rows = SelectedLibraryRows();
    const LibraryRow*              rp = rows.size() == 1 ? &rows.front() : nullptr;
    const std::string              lib = SelectedLibraryUid();
    std::string                    label;
    const bool can = !picks.empty() && !g.pending &&
                     MediaUseLabel(GetSoundStore(nullptr), lib, rp, static_cast<int>(picks.size()), false, &label);
    if (ImGui::MenuItem(can ? label.c_str() : "Use the selection", nullptr, false, can)) UseAtTarget(picks);
    if (ImGui::MenuItem("Make a Random of the selection", nullptr, false, can && picks.size() >= 2))
        UseAtTarget(picks, true);
    ui::MenuSeparator();
    if (ImGui::MenuItem("Select all results", "Ctrl+A", false, !g.results.empty())) {
        SelRows(MediaRangeRows(0, static_cast<int>(std::min(g.results.size(), kMaxResults)) - 1, g.results.size(),
                               kMaxResults),
                false);
        g.vsel.clear();
    }
    ui::EndMenuPopup();
}

// A result row's height (.frow.res: pad 3 8 4 around a 12 px line), rows flush.
float ResultRowH()
{
    return std::round(std::max(ImGui::GetTextLineHeight(), ui::FontSemibold()->FontSize) + ui::Dp(7.0f));
}

// The results (.res-list): a dark box (kBg, 1 px stroke, r5), one flush row per file: the star
// (.fav: faint, warn when on or under the mouse) and the name in semibold 12 (ellipsized, faint
// when missing). Rows: every other one kRowAlt, hover raised, selected accent-soft with a 2 px
// accent bar, the previewed file (not selected) a 2 px accent-line bar.
void DrawResults(float h)
{
    if (!ui::BeginListPane("##meresults", ImVec2(0.0f, h))) {  // kBg, 1 px stroke, r5, rows inside the edge
        ui::EndListPane();
        return;
    }
    const int ndb = static_cast<int>(g.info.databases.size());
    if (g.results.empty()) {
        std::string hint;
        if (!MediaDbReady()) hint = "Loading the media databases...";
        else if (g.info.databases.empty()) hint = "No media database found: build one in REAPER's Media Explorer";
        else if (g.filter == ndb + 1) hint = "No favourite matches";
        else if (g.query[0] == 0) hint = "No file in this database";
        else hint = std::string("No file matches \"") + g.query + "\"";
        ImGui::SetCursorPos(ImVec2(ui::Dp(8.0f), ui::Dp(6.0f)));
        ui::ToneText(hint, ui::kMuted, true, std::max(1.0f, ImGui::GetContentRegionAvail().x - ui::Dp(8.0f)));
        ui::EndListPane();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont*     bf = ui::FontSemibold();
    const float rh = ResultRowH();  // one line: star + name (the file's info sits above the waveform)
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));  // rows flush
    if (g.scroll_to >= 0) {  // the keyboard row is kept in view (the clipper skips hidden rows)
        const float y = g.scroll_to * rh, top = ImGui::GetScrollY(), vh = ImGui::GetWindowHeight();
        const float pad = ImGui::GetStyle().WindowPadding.y * 2.0f;
        if (y < top) ImGui::SetScrollY(y);
        else if (y + rh > top + vh - pad) ImGui::SetScrollY(y + rh - vh + pad);
        g.scroll_to = -1;
    }
    const float star_x = ui::Dp(17.0f), star_hit = ui::Dp(26.0f), name_dx = ui::Dp(29.0f);
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(g.results.size()) + (g.capped ? 1 : 0), rh);
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            if (i >= static_cast<int>(g.results.size())) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ui::EllipsisText(dl, ImVec2(p.x + ui::Dp(8.0f), p.y + (rh - ImGui::GetTextLineHeight()) * 0.5f),
                                 p.x + w - ui::Dp(8.0f), ui::kMuted, "1000+ files: type more words to narrow");
                ImGui::Dummy(ImVec2(w, rh));
                continue;
            }
            const DbFile& f = *g.results[i];
            g.exists.Want(f.path);  // checked after the frame, with the other rows on screen
            const bool    miss = IsMissing(f.path);
            const bool    sel = IsSelected(f.path);
            const bool    cur = g.has_cur && g.cur.path == f.path;  // the previewed file = the keyboard row
            ImGui::PushID(i);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            // The row (.frow.res): hover raised, selected = a 2 px accent bar, the previewed file a
            // 2 px accent-line bar, every other row kRowAlt.
            const unsigned bits = ui::kRowBarSelect | (!sel && cur ? ui::kRowCurrent : 0u) | ((i % 2) ? ui::kRowEven : 0u);
            const bool     clicked = ui::SelectRow("##r", sel, bits, rh, ImGuiSelectableFlags_AllowDoubleClick, 0.0f);
            const bool     hov = ImGui::IsItemHovered();
            const bool dbl = clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            const bool on_star = ImGui::GetIO().MousePos.x < p.x + star_hit;
            if (clicked && on_star) {
                if (!dbl) ToggleFav(f.path);  // the double-click's second press would undo the first
            } else if (dbl && !miss) {
                UseAtTarget({WholePick(f)});
            } else if (clicked) {
                bool added = true;
                const ImGuiIO& io = ImGui::GetIO();
                if (io.KeyShift) {  // 13-69: a range from the anchor (Ctrl+Shift adds it)
                    SelRows(MediaRangeRows(g.anchor_row, i, g.results.size(), kMaxResults), io.KeyCtrl);
                    if (g.anchor_row < 0) g.anchor_row = i;
                } else if (io.KeyCtrl) {
                    added = SelToggle(f);  // un-selected: neither shown nor played
                    g.anchor_row = i;
                } else {
                    SelSet({f});
                    g.anchor_row = i;
                }
                g.vsel.clear();
                if (added) {
                    SetCurrent(f);
                    g.cur_row = i;
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
                // Antho 2026-10-19: the previewed file's row drags its chosen variations (one Sound of them).
                if (g.has_cur && g.cur.path == f.path && !g.vsel.empty() && g.an.ready)
                    picks = MediaListDragPicks(f.path, picks, SlicePicks(g.vsel));
                BeginDrag(picks, MediaDragLabel(picks));
            }
            // 13-69: right-click = the selection's menu (an unselected row becomes the selection).
            if (hov && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                if (!IsSelected(f.path)) {
                    SelSet({f});
                    g.anchor_row = i;
                }
                g_open_ctx = true;  // opened at the list's ID level (DrawResultsMenu)
            }
            // 13-69: where the search's words were found (a hit from a folder or a tag is understood).
            if (hov && !on_star && !ImGui::GetDragDropPayload() && g.index) {
                const std::vector<std::string> where = g.index->MatchedFields(&f, g.searched);
                if (!where.empty()) {
                    std::string tip = "Found in: ";
                    for (size_t k = 0; k < where.size(); ++k) tip += (k ? ", " : "") + where[k];
                    ui::ItemTipText(tip.c_str());
                }
            }
            DrawStar(dl, ImVec2(p.x + star_x, std::floor(p.y + rh * 0.5f) + 0.5f), ui::Dp(5.0f), IsFav(f.path),
                     hov && on_star ? ui::kWarn : ui::kFaint);
            {
                ui::FontScope fs(bf);
                ui::EllipsisText(dl, ImVec2(p.x + name_dx, std::floor(p.y + (rh - bf->FontSize) * 0.5f)),
                                 p.x + w - ui::Dp(8.0f), miss ? ui::kFaint : ui::kText, MediaFileName(f.path));
            }
            ImGui::PopID();
        }
    }
    ImGui::PopStyleVar();
    DrawResultsMenu();
    ui::EndListPane();
}

// The current file's status, empty when it is ready (Missing / Reading / Opening).
const char* PreviewStatus()
{
    if (IsMissing(g.cur.path)) return "Not found on this PC";
    if (!g.an.ready) return "Reading...";
    if (g_pv.opening && g_pv.open_path == g.cur.path) return "Opening...";
    return "";
}

// The empty preview's hint (.sub) above an empty 60 px box.
const char* kPreviewHint = "Click a result to see and hear it";

// The current file's info, above the waveform (DECISIONS section 6, the mockup's typography): line 1 the
// name in semibold 12 (ellipsized; the gear at the right, drawn by the caller); then the status
// (11, muted; missing in the miss colour), then format (mono 10 faint) - length (mono 11 muted) -
// "(database / library)" (11 faint), each part on the line while it fits, else on the next one
// (ellipsized when even a line of its own is too short); then the description (11 muted,
// wrapped). Measures when `dl` is null, so the panel's height reserve is what is drawn. Returns
// the block's height from y.
float LayoutInfo(ImDrawList* dl, float x, float y, float w)
{
    const DbFile& f = g.cur;
    const bool    miss = IsMissing(f.path);
    const float   gear = ui::ControlH(), gap = ui::Dp(3.0f);
    const float   y0 = y;
    w = std::max(1.0f, w);
    ImFont* bf = ui::FontSemibold();
    if (dl) {
        ui::FontScope fs(bf);
        ui::EllipsisText(dl, ImVec2(x, std::floor(y + (gear - bf->FontSize) * 0.5f)), x + w - gear - ui::Dp(6.0f),
                         miss ? ui::kFaint : ui::kText, MediaFileName(f.path));
    }
    y += gear;
    ImFont* sf = ui::FontSmall();
    auto wrapped = [&](const char* t, ImU32 col) {
        if (!t || !*t) return;
        y += gap;
        if (dl) dl->AddText(sf, sf->FontSize, ImVec2(x, y), col, t, nullptr, w);
        y += sf->CalcTextSizeA(sf->FontSize, FLT_MAX, w, t).y;
    };
    wrapped(PreviewStatus(), miss ? ui::kMissText : ui::kMuted);
    // The meta line: parts that flow and wrap as words.
    struct Part {
        std::string text;
        ImFont*     font;
        ImU32       col;
    };
    std::vector<Part> parts;
    const std::string fmt = FormatMediaFormat(f);
    if (!fmt.empty()) parts.push_back({fmt, ui::FontMonoTiny(), ui::kFaint});
    const double len = g.an.length > 0.0 ? g.an.length : f.length;
    const std::string ls = len >= 0.0 ? FormatMediaLength(len) : std::string();
    if (!ls.empty()) parts.push_back({ls, ui::FontMonoSmall(), ui::kMuted});
    std::string src = f.db;
    if (!f.library.empty()) src += (src.empty() ? "" : " / ") + f.library;
    if (!src.empty()) parts.push_back({"(" + src + ")", sf, ui::kFaint});
    if (!parts.empty()) {
        float line_h = 0.0f;
        for (const Part& p : parts) line_h = std::max(line_h, p.font->FontSize);
        y += gap;
        float       cx = x;
        const float sep = ui::Dp(8.0f);
        for (const Part& p : parts) {
            const float pw = ui::TextSizeIn(p.font, p.text.c_str()).x;
            if (cx > x && cx + sep + pw > x + w) {  // does not fit after the previous part: next line
                y += line_h;
                cx = x;
            } else if (cx > x) {
                cx += sep;
            }
            if (dl) {
                ui::FontScope fs(p.font);
                ui::EllipsisText(dl, ImVec2(cx, std::floor(y + (line_h - p.font->FontSize) * 0.5f)), x + w, p.col, p.text);
            }
            cx += std::min(pw, x + w - cx);
        }
        y += line_h;
    }
    wrapped(f.description.c_str(), ui::kMuted);
    return y - y0;
}

float PreviewInfoHeight(float w)
{
    if (!g.has_cur) return ImGui::CalcTextSize(kPreviewHint, nullptr, false, w).y;
    return LayoutInfo(nullptr, 0.0f, 0.0f, w);
}

void DrawPreview(float wave_h)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!g.has_cur) {
        ui::SubText(kPreviewHint);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        DrawMediaWaveform(dl, p, ImVec2(p.x + w, p.y + wave_h), Analysis{}, {}, -1.0);
        ImGui::Dummy(ImVec2(w, wave_h));
        return;
    }
    const bool   miss = IsMissing(g.cur.path);
    const ImVec2 np = ImGui::GetCursorScreenPos();
    const float  left = np.x;
    const float  iw = std::max(1.0f, ui::LineRight() - left);
    const float  info_h = LayoutInfo(dl, left, np.y, iw);
    // The gear (.btn.sm.ico gear glyph), right-aligned on the name's line: the detection defaults.
    {
        const SilenceParams sp{};
        char tip[320];
        std::snprintf(tip, sizeof tip,
                      "Detection defaults for new sounds: split on silence, threshold %.0f dB, min gap %.0f ms, "
                      "pre-roll %.0f ms, tail %.0f ms (each sound can then be pre-edited in its panel)",
                      sp.threshold_db, sp.min_gap_ms, sp.preroll_ms, sp.tail_ms);
        ImGui::SetCursorScreenPos(ImVec2(left + iw - ui::ControlH(), np.y));
        ui::IconOnlyButton("##megear", "\xE2\x9A\x99", tip);
    }
    ImGui::SetCursorScreenPos(np);
    ImGui::Dummy(ImVec2(iw, info_h));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 p1(p.x + w, p.y + wave_h);
    const double ph = (g_pv.playing && g_pv.path == g.cur.path) ? PreviewFileTime() : -1.0;
    DrawMediaWaveform(dl, p, p1, g.an, g.vsel, ph, miss ? -1 : HoveredFrame(p, p1, g.an));
    ImGui::InvisibleButton("##mewave", ImVec2(w, wave_h));
    if (miss) {
        ui::ItemTipText("Not found on this PC");
        return;
    }
    ui::ItemTipText("Click a variation to play it, Ctrl-click to select several, click between them to play from "
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
        if (!picks.empty()) BeginDrag(picks, MediaDragLabel(picks));
    }
}

// The footer (.me-foot): a divider across the card, then 6 px under it the use button at its
// natural width, left-aligned (or the reading line with Cancel). Always one ControlH() line, so
// the list above keeps its height.
float FooterHeight() { return 1.0f + ui::Dp(6.0f) + ui::ControlH(); }

void DrawFooter(const ui::Section& s)
{
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImVec2 c = ImGui::GetCursorScreenPos();
    const float  dy = std::floor(c.y) + 0.5f;
    dl->AddLine(ImVec2(s.card.p0.x + 1.0f, dy), ImVec2(s.card.p0.x + s.card.w - 1.0f, dy), ui::kStroke, 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(c.x, std::floor(c.y) + 1.0f + ui::Dp(6.0f)));
    const float line_y = ImGui::GetCursorScreenPos().y;
    auto line_text = [&](const char* t) {  // one muted line, ellipsized (its full text as a tooltip)
        ImGui::AlignTextToFramePadding();
        ui::FitText(t, ui::kMuted);
    };
    if (g.pending) {  // a use waiting for its files' variations
        const std::string msg = MediaReadingLabel(MediaBatchLeft(g.batch));
        const float       cw = ui::ButtonWidth("Cancel");
        const float       room = ImGui::GetContentRegionAvail().x;
        ui::EllipsisText(dl, ImVec2(c.x, line_y + ImGui::GetStyle().FramePadding.y), c.x + room - cw - ui::Dp(8.0f),
                         ui::kMuted, msg);
        ImGui::SetCursorScreenPos(ImVec2(c.x + std::max(0.0f, room - cw), line_y));
        if (ui::SolidButton("Cancel")) CancelPending();
        ui::ItemTipText("Stop reading: no sound is added");
        return;
    }
    bool                   variations = false;
    std::vector<MediaPick> picks = UsedPicks(&variations);
    if (picks.empty()) {
        ImGui::Dummy(ImVec2(1.0f, ui::ControlH()));
        return;
    }
    const std::vector<LibraryRow>& rows = SelectedLibraryRows();
    const LibraryRow*              rp = rows.size() == 1 ? &rows.front() : nullptr;
    const std::string              lib = SelectedLibraryUid();
    std::string                    label;
    const SoundStore&              store = GetSoundStore(nullptr);
    if (lib.empty() || !FindLibrary(store, lib)) {
        line_text("Create a library first");
        return;
    }
    if (!MediaUseLabel(store, lib, rp, static_cast<int>(picks.size()), variations, &label)) {
        line_text("Pick a value row of this Switch first");
        return;
    }
    if (ui::SolidButton(label.c_str())) UseAtTarget(picks);
    // 13-69: the selection counted, after the button (ellipsized when the column is narrow).
    const std::string count = variations ? std::string() : MediaSelectedLabel(g.sel.size());
    if (!count.empty()) {
        ImGui::SameLine();
        line_text(count.c_str());
    }
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
    if (io.KeyCtrl && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_A, false) && !g.results.empty()) {  // 13-69: every result
        SelRows(MediaRangeRows(0, static_cast<int>(std::min(g.results.size(), kMaxResults)) - 1, g.results.size(),
                               kMaxResults),
                false);
        g.vsel.clear();
        return;
    }
    const bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow), down = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
    if ((up || down) && !g.results.empty()) {  // the keyboard row = the previewed file
        int at = -1;
        if (g.has_cur && g.cur_row >= 0 && g.cur_row < static_cast<int>(g.results.size()) &&
            g.results[static_cast<size_t>(g.cur_row)]->path == g.cur.path)
            at = g.cur_row;  // O(1): the row kept on click / arrow
        else if (g.has_cur)
            for (size_t i = 0; i < g.results.size(); ++i)
                if (g.results[i]->path == g.cur.path) {
                    at = static_cast<int>(i);
                    break;
                }
        const int n = static_cast<int>(g.results.size());
        const int to = at < 0 ? 0 : std::clamp(at + (down ? 1 : -1), 0, n - 1);
        if (to != at) {
            const DbFile& f = *g.results[static_cast<size_t>(to)];
            SelSet({f});
            g.anchor_row = to;
            g.vsel.clear();
            SetCurrent(f);
            g.cur_row = to;
            if (IsMissing(f.path)) StopPreviewNow();
            else if (g.autoplay) PlayFile(f);
            g.scroll_to = to;
        }
        return;
    }
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
    f_play_ex = reinterpret_cast<PlayPreviewExFn>(get_func("PlayPreviewEx"));
    f_stop = reinterpret_cast<StopPreviewFn>(get_func("StopPreview"));
    f_create = reinterpret_cast<CreateFromFileFn>(get_func("PCM_Source_CreateFromFile"));
    ApplyHooks();  // the drop targets know the payload before the panel first draws
}

void StopMediaPreview() { StopPreviewNow(); }

void MediaReachAsk(const std::string& path, bool urgent)
{
    if (path.empty() || g_quitting) return;
    if (!g_reach_inflight.insert(path).second) return;
    Task t;
    t.kind = TaskKind::Reach;
    t.path = path;
    t.owner = PeakOwner::Audition;
    g_probe.Push(std::move(t), urgent);  // urgent: ahead of the queued background checks
}

bool MediaReachLast(const std::string& path, bool* ok, long long* size, double* age_s)
{
    int64_t sz = -1;
    const bool known = g_reach.Last(path, ok, &sz, age_s, ReachNow());
    if (size) *size = sz;
    return known;
}

int MediaReachInflight() { return static_cast<int>(g_reach_inflight.size()); }

int MediaReachGet(const std::string& path) { return static_cast<int>(g_reach.Get(path, ReachNow())); }

void MediaReachPoll() { PollWorkers(); }

bool MediaPanelTakesArrows() { return g.takes_arrows; }

void QuitLibraryMedia()
{
    StopPreviewNow();
    CancelPending();
    // Both workers stop (a check in progress sees g_quitting) and are joined (the peak reader is
    // sound_peaks', stopped by QuitSoundPeaks).
    g_quitting = true;
    g_rows.Stop();
    g_probe.Stop();
    g_reach_inflight.clear();
    g.exists.Clear();
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
    RAV_QUIET_SCOPE("Media Explorer", nullptr);  // freeze audit
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
            g.dirty = true;  // the files already checked stay known (Reload clears them)
        }
    }
    if (g.edited_at >= 0.0 && ImGui::GetTime() - g.edited_at >= kDebounce) {
        g.edited_at = -1.0;
        g.dirty = true;
    }
    if (g.dirty && MediaDbReady()) RunSearch();

    ImGui::PushID("##media");
    // 13-7c (audit C7): the whole panel is one fixed section card (.secx.me) that fills the column:
    // "Media Explorer" with the databases' status as its faint suffix, the header's buttons at the
    // right; then search, results, the file's info, the waveform and the footer.
    const float top_y = ImGui::GetCursorScreenPos().y;
    const float avail_h = ImGui::GetContentRegionAvail().y;
    const int   nbtn = HeaderButtons(ImGui::GetContentRegionAvail().x - 2.0f * ui::kCardPad);
    const std::string status = HeaderStatus();
    ui::SectionOpts so;
    so.foldable = false;
    so.suffix = status.c_str();
    so.right_w = HeaderButtonsWidth(nbtn);
    ui::Section sec;
    ui::BeginSection(sec, "Media Explorer", so);
    ui::BeginSectionHeaderRight(sec);
    DrawHeaderButtons(nbtn);
    ui::EndSectionHeaderRight(sec);
    DrawSearch();
    // The list takes what is left once the info, the waveform (120 px; 96 when the column is short;
    // the empty box 60) and the footer have their room, the card's bottom padding included.
    const float sp = ImGui::GetStyle().ItemSpacing.y;
    const float row_h = ResultRowH();
    const float info_h = PreviewInfoHeight(std::max(1.0f, ui::LineRight() - ImGui::GetCursorScreenPos().x));
    const float room = top_y + avail_h - ui::kCardPad - ui::Dp(2.0f) - ImGui::GetCursorScreenPos().y - 3.0f * sp -
                       info_h - FooterHeight();
    float wave_h = g.has_cur ? ui::Dp(120.0f) : ui::Dp(60.0f);
    if (g.has_cur && room - wave_h < row_h * 4.0f + 2.0f) wave_h = ui::Dp(96.0f);
    const float list_h = std::max(row_h * 4.0f + 2.0f, room - wave_h);
    DrawResults(list_h);
    CheckVisibleRows();
    g.takes_arrows = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput && !g.results.empty();
    DrawPreview(wave_h);
    DrawFooter(sec);
    ui::EndSection(sec);
    HandleKeys();
    // A preview that reached its end is released even when the waveform is another file's.
    if (g_pv.playing) PreviewFileTime();
    ImGui::PopID();
}

}  // namespace rav

#endif  // _WIN32
