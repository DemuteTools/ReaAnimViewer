// SPDX-License-Identifier: MIT
//
// See sound_peaks.h. The peaks are read from the file's own samples (a PCM_source made with
// PCM_Source_CreateFromFile, read with GetSamples on the main thread, a bounded slice per call):
// no peak file is written and nothing waits on REAPER's peak builder.

#include "sound_peaks.h"

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>

#include "sound_populate.h"

namespace rav {
namespace {

using CreateFromFileFn = PCM_source* (*)(const char*);
using GetNumTakeMarkersFn = int (*)(MediaItem_Take*);
using GetTakeMarkerFn = double (*)(MediaItem_Take*, int, char*, int, int*);
using GetItemStateChunkFn = bool (*)(MediaItem*, char*, int, bool);
using CountTakesFn = int (*)(MediaItem*);
using GetMediaItemTakeFn = MediaItem_Take* (*)(MediaItem*, int);
using FileForReadFn = bool (*)(char*, const char*, const char*);

CreateFromFileFn    f_create = nullptr;
GetNumTakeMarkersFn f_num_tm = nullptr;
GetTakeMarkerFn     f_get_tm = nullptr;
GetItemStateChunkFn f_chunk = nullptr;
CountTakesFn        f_count_takes = nullptr;
GetMediaItemTakeFn  f_get_take = nullptr;
FileForReadFn       f_file_read = nullptr;

constexpr size_t kCacheFiles = 32;
constexpr double kBudgetMs = 8.0;   // per window: every call of one UI frame shares it
constexpr double kWindowMs = 16.0;
constexpr int    kBlock = 8192;

struct Entry {
    PeakState                   state = PeakState::Building;
    PeakData                    peaks;
    double                      length = 0.0;
    uint64_t                    used = 0;
    std::unique_ptr<PCM_source> src;  // while building
    double                      sr = 0.0;
    int                         nch = 0;
    int64_t                     done = 0;  // samples read
    int64_t                     total = 0;
};

// Never destroyed: a building entry's REAPER source is not deleted from static destructors at
// unload (the process is going away; REAPER may already be gone).
std::map<std::string, Entry>& g_cache = *new std::map<std::string, Entry>();
uint64_t                     g_tick = 0;
std::chrono::steady_clock::time_point g_win_start{};
double                                g_win_spent = 0.0;
std::map<std::string, double> g_len;

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

bool Open(const std::string& path, Entry& e)
{
    if (!f_create || path.empty()) return false;
    PCM_source* s = f_create(path.c_str());
    if (!s) return false;
    e.src.reset(s);
    e.sr = s->GetSampleRate();
    e.nch = s->GetNumChannels();
    e.length = s->GetLength();
    if (e.sr < 1.0 || e.nch < 1 || !(e.length > 0.0)) {
        e.src.reset();
        return false;
    }
    e.total = static_cast<int64_t>(std::ceil(e.length * e.sr));
    e.peaks.rate = kPeakRate;
    PeakInit(&e.peaks, e.nch, static_cast<size_t>(std::ceil(e.length * kPeakRate)) + 1);  // lanes: stereo only
    g_len[path] = e.length;
    return true;
}

// Reads blocks until the budget is spent; true when the file is done.
bool Run(Entry& e, double budget_ms)
{
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<ReaSample> buf(static_cast<size_t>(kBlock) * static_cast<size_t>(e.nch));
    while (e.done < e.total) {
        PCM_source_transfer_t tr = {};
        tr.time_s = static_cast<double>(e.done) / e.sr;
        tr.samplerate = e.sr;
        tr.nch = e.nch;
        tr.length = static_cast<int>(std::min<int64_t>(kBlock, e.total - e.done));
        tr.samples = buf.data();
        e.src->GetSamples(&tr);
        const int got = tr.samples_out > 0 ? std::min(tr.samples_out, tr.length) : 0;
        for (int k = 0; k < got; ++k) {
            const size_t bin = static_cast<size_t>(static_cast<double>(e.done + k) * kPeakRate / e.sr);
            PeakAddFrame(&e.peaks, bin, buf.data() + static_cast<size_t>(k) * static_cast<size_t>(e.nch), e.nch);
        }
        e.done += tr.length;  // a short read is silence: never stall on it
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > budget_ms) break;
    }
    return e.done >= e.total;
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

// Advances the one building entry within what is left of the window's budget (one UI frame
// calls RequestPeaks many times: together they spend kBudgetMs at most).
void Advance(Entry& e)
{
    if (e.state != PeakState::Building) return;
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double, std::milli>(now - g_win_start).count() > kWindowMs) {
        g_win_start = now;
        g_win_spent = 0.0;
    }
    const double left = kBudgetMs - g_win_spent;
    if (left <= 0.0) return;
    const bool done = Run(e, left);
    g_win_spent += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - now).count();
    if (done) {
        e.state = PeakState::Ready;
        e.src.reset();
    }
}

}  // namespace

void InitSoundPeaks(void* (*get_func)(const char* name))
{
    if (!get_func) return;
    f_create = reinterpret_cast<CreateFromFileFn>(get_func("PCM_Source_CreateFromFile"));
    f_num_tm = reinterpret_cast<GetNumTakeMarkersFn>(get_func("GetNumTakeMarkers"));
    f_get_tm = reinterpret_cast<GetTakeMarkerFn>(get_func("GetTakeMarker"));
    f_chunk = reinterpret_cast<GetItemStateChunkFn>(get_func("GetItemStateChunk"));
    f_count_takes = reinterpret_cast<CountTakesFn>(get_func("CountTakes"));
    f_get_take = reinterpret_cast<GetMediaItemTakeFn>(get_func("GetMediaItemTake"));
    f_file_read = reinterpret_cast<FileForReadFn>(get_func("GetUserFileNameForRead"));
}

PeakState RequestPeaks(const std::string& path, PeakData* out)
{
    if (path.empty() || !f_create) return PeakState::Failed;
    auto it = g_cache.find(path);
    if (it == g_cache.end()) {
        // One file builds at a time: the others wait their turn, and asking advances it (a file
        // nobody shows any more still finishes).
        for (auto& kv : g_cache)
            if (kv.second.state == PeakState::Building) {
                Advance(kv.second);
                if (kv.second.state == PeakState::Building) return PeakState::Building;
            }
        Entry e;
        if (!Open(path, e)) e.state = PeakState::Failed;
        it = g_cache.emplace(path, std::move(e)).first;
    }
    Entry& e = it->second;
    e.used = ++g_tick;
    Advance(e);
    if (e.state == PeakState::Ready && out) *out = e.peaks;
    const PeakState st = e.state;
    Evict();
    return st;
}

bool ReadPeaks(const std::string& path, double start, double length, double rate, PeakData* out)
{
    if (out) *out = PeakData{};
    if (!out || !(length > 0.0) || !(rate > 0.0)) return false;
    if (RequestPeaks(path, nullptr) != PeakState::Ready) return false;
    const PeakData& p = g_cache[path].peaks;
    const size_t    n = static_cast<size_t>(std::max(1.0, std::floor(length * rate)));
    const int       lanes = PeakChannels(p);
    PeakInit(out, lanes, n);
    out->rate = rate;
    for (size_t i = 0; i < n; ++i) {
        const double a = (start + static_cast<double>(i) / rate) * kPeakRate;
        const double b = (start + static_cast<double>(i + 1) / rate) * kPeakRate;
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
    auto it = g_len.find(path);
    if (it != g_len.end()) return it->second;
    double len = 0.0;
    if (f_create) {
        if (PCM_source* s = f_create(path.c_str())) {
            len = s->GetLength();
            delete s;
        }
    }
    if (len > 0.0) g_len[path] = len;  // a missing file is looked for again (restored, located)
    return len;
}

bool ReadItemMedia(MediaItem* item, SoundMedia* out)
{
    if (!out) return false;
    *out = SoundMedia{};
    if (!item) return false;
    MediaItem_Take* active = GetActiveTake(item);
    out->item_pos = GetMediaItemInfo_Value(item, "D_POSITION");
    out->facts.item_len = GetMediaItemInfo_Value(item, "D_LENGTH");
    {
        const double vol = GetMediaItemInfo_Value(item, "D_VOL");  // the item's gain: its variations' (DetectFrames)
        out->facts.item_db = vol > 0.0 ? 20.0 * std::log10(vol) : -60.0;
    }
    const int n = f_count_takes ? f_count_takes(item) : (active ? 1 : 0);
    for (int k = 0; k < n; ++k) {
        MediaItem_Take* t = f_get_take ? f_get_take(item, k) : active;
        double          len = 0.0;
        PCM_source*     src = t ? GetMediaItemTake_Source(t) : nullptr;
        std::string     path = src ? RootFileOf(src, &len) : std::string();
        out->take_paths.push_back(path);
        out->take_lens.push_back(len);
        if (t == active) out->active_take = k;
        if (n >= 2) {
            SoundFrame fr;
            fr.start = t ? GetMediaItemTakeInfo_Value(t, "D_STARTOFFS") : 0.0;
            // 13-24 review: the item's length in this take's source time (its playrate), inside its
            // source; 0 = the item's length (DetectFrames).
            const double rate = t ? GetMediaItemTakeInfo_Value(t, "D_PLAYRATE") : 1.0;
            double       fl = out->facts.item_len * ((std::isfinite(rate) && rate > 0.0) ? rate : 1.0);
            if (len > 0.0) fl = std::min(fl, len - fr.start);
            fr.length = fl > 0.0 ? fl : 0.0;
            out->facts.takes.push_back(fr);
        }
    }
    if (!active || out->take_paths.empty()) return false;
    out->path = out->take_paths[static_cast<size_t>(out->active_take)];
    out->source_len = out->take_lens[static_cast<size_t>(out->active_take)];
    out->facts.source_len = out->source_len;
    out->facts.start_offs = GetMediaItemTakeInfo_Value(active, "D_STARTOFFS");
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
        std::vector<char> buf(1 << 20);
        if (f_chunk(item, buf.data(), static_cast<int>(buf.size()), false)) {
            const std::vector<Slice> s = SlicesFromRppText(std::string(buf.data()));
            if (s.size() >= 2) out->facts.slices = s;
        }
    }
    out->ok = !out->path.empty();
    return out->ok;
}

bool ReadSoundMedia(const SoundSource& source, SoundMedia* out)
{
    if (!out) return false;
    *out = SoundMedia{};
    if (source.kind == SoundSourceKind::Item) {
        MediaItem* item = source.item_guid.empty() ? nullptr : FindItemByGuid(source.item_guid);
        return item ? ReadItemMedia(item, out) : false;
    }
    const std::string& path = source.asset.last_path;
    const double       len = SourceFileLength(path);
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

bool ChooseSoundFile(const std::string& title, std::string* path)
{
    if (!f_file_read || !path) return false;
    std::vector<char> buf(4096, '\0');
    if (!path->empty()) std::snprintf(buf.data(), buf.size(), "%s", path->c_str());
    if (!f_file_read(buf.data(), title.c_str(), "")) return false;  // any audio type, not only .wav
    *path = buf.data();
    return !path->empty();
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
