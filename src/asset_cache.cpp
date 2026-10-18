// SPDX-License-Identifier: MIT
//
// See asset_cache.h.

#include "asset_cache.h"

#ifdef _WIN32

#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "asset_lru.h"
#include "console_log.h"
#include "evict_generation.h"

namespace rav {
namespace {

// What identifies one version of a file: a re-export changes its date (and usually its
// size). Both zero when the file cannot be read (never equal to a real stamp... unless
// it stays unreadable, which is the same "version").
using FileStamp = AssetFileStamp;

FileStamp StampOf(const std::string& path)
{
    FileStamp st;
    try {
        std::error_code ec;
        const std::filesystem::path p = std::filesystem::u8path(path);
        const auto t = std::filesystem::last_write_time(p, ec);
        if (!ec) st.time = static_cast<long long>(t.time_since_epoch().count());
        const auto sz = std::filesystem::file_size(p, ec);
        if (!ec) st.size = static_cast<unsigned long long>(sz);
    } catch (...) {
        // u8path can throw on a malformed string: treat as unreadable.
    }
    return st;
}

struct Entry {
    std::shared_ptr<const CpuAsset> asset;   // null = the load failed
    FileStamp stamp;                         // the file's stamp when it was read
    uint64_t  last_use = 0;                  // freeze fix #1: g_use when last handed out
    size_t    bytes = 0;                     // its approximate size in memory
    // Freeze fix #2: why the load failed (the viewer's message), when it did.
    LoadErrorCategory category = LoadErrorCategory::Ok;
    std::string       detail;
    std::string       hint;
};

std::mutex                              g_mutex;
std::unordered_map<std::string, Entry>  g_entries;  // guarded by g_mutex
std::unordered_set<std::string>         g_wanted;   // guarded by g_mutex
EvictGenerations                        g_evicted;  // guarded by g_mutex (13-51)
uint64_t                                g_use = 0;  // guarded by g_mutex: the use counter

// Approximate memory of one parse (its vertices, indices, images and keys): the bound of the
// recent files kept beside the video FX's (asset_lru.h).
size_t ApproxBytes(const CpuAsset* a)
{
    if (!a) return 64;
    size_t n = sizeof(CpuAsset);
    for (const CpuMesh& m : a->meshes) n += m.verts.size() * sizeof(SceneVertex) + m.indices.size() * sizeof(uint32_t);
    for (const CpuMaterial& m : a->materials)
        for (const CpuImage* im : {&m.baseColor, &m.normalMap, &m.specularMap, &m.glossMap})
            n += im->encoded.size() + im->rgba.size();
    n += a->skeleton.bones.size() * sizeof(SceneBone);
    for (const SceneAnimation& an : a->animations)
        for (const AnimChannel& ch : an.channels)
            n += ch.translation.size() * sizeof(KeyframeT) + ch.rotation.size() * sizeof(KeyframeR) +
                 ch.scale.size() * sizeof(KeyframeS) + sizeof(AnimChannel);
    return n;
}

// Freeze fix #1: drops the least recently used parses no video FX track uses, beyond the bounds
// (asset_lru.h). Called with g_mutex held.
void EnforceBoundsLocked()
{
    std::vector<LruCandidate>                 c;
    std::vector<decltype(g_entries)::iterator> its;
    c.reserve(g_entries.size());
    its.reserve(g_entries.size());
    for (auto it = g_entries.begin(); it != g_entries.end(); ++it) {
        c.push_back(LruCandidate{it->second.last_use, it->second.bytes, g_wanted.count(it->first) != 0});
        its.push_back(it);
    }
    for (const size_t i : LruEvictions(c, kAssetLruMaxCount, kAssetLruMaxBytes)) g_entries.erase(its[i]);
}

}  // namespace

AssetFileStamp ReadAssetFileStamp(const std::string& path)
{
    return StampOf(path);
}

namespace {

// The cache's lookup. `expect` (optional): the file's version the caller just read on disk; an
// entry of another version is parsed again (freeze fix #2: the cold paths, the viewer and the
// Tagging view, never get a parse older than the file). `used`: the version handed out.
CpuLoadResult Lookup(const std::string& path, const FileStamp* expect, FileStamp* used)
{
    if (used) *used = FileStamp{};
    if (path.empty()) return {nullptr, LoadErrorCategory::FileNotFound, "no file", "No file."};
    try {
        uint64_t gen = 0;  // the path's eviction generation when this parse starts
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const auto it = g_entries.find(path);
            if (it != g_entries.end() && (!expect || it->second.stamp == *expect)) {
                Entry& e = it->second;
                e.last_use = ++g_use;
                if (used) *used = e.stamp;
                return {e.asset, e.category, e.detail, e.hint};
            }
            gen = g_evicted.Of(path);
        }
        // Miss: parse here, outside the lock (seconds for a big FBX), so other threads'
        // lookups never wait on it. If two threads miss the same file at once both parse
        // it; the last one stored wins, both results are valid.
        const FileStamp stamp = StampOf(path);
        if (used) *used = stamp;
        const auto t0 = std::chrono::steady_clock::now();
        CpuLoadResult r = LoadCpuAsset(path);
        // The parse blocks the calling thread (REAPER's video thread on a miss): logged so
        // the debuglog gate sees that stall next to the per-frame cost lines.
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!r.asset) {
            LogError("parse failed [%s] %s: %s", LoadErrorCategoryName(r.category), path.c_str(),
                     r.detail.c_str());
        } else {
            LogInfo("parsed %s in %.0f ms", path.c_str(), ms);
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        // 13-51: evicted during the parse ("Locate textures..." copied textures meanwhile): this
        // parse may lack them, so it is not stored; the next lookup parses the file again. The
        // caller still draws it this once (a complete model, just maybe flat).
        if (ParseStillCurrent(gen, g_evicted.Of(path))) {
            Entry& e = g_entries[path];
            // Freeze fix #2: the same version parsed meanwhile by another thread is kept (a new
            // parse object would make the video thread drop and re-upload its GPU copy).
            if (e.asset && r.asset && e.stamp == stamp) {
                e.last_use = ++g_use;
                return {e.asset, e.category, e.detail, e.hint};
            }
            e = Entry{r.asset, stamp, ++g_use, ApproxBytes(r.asset.get()), r.category, r.detail, r.hint};
            EnforceBoundsLocked();
        }
        return r;
    } catch (...) {
        // bad_alloc on the map: no picture, try again next frame
        return {nullptr, LoadErrorCategory::OutOfMemory, "out of memory", "Not enough memory to open this file."};
    }
}

}  // namespace

std::shared_ptr<const CpuAsset> AcquireCpuAsset(const std::string& path)
{
    return Lookup(path, nullptr, nullptr).asset;
}

std::shared_ptr<const CpuAsset> AcquireCpuAsset(const std::string& path, AssetFileStamp* stamp_out)
{
    return Lookup(path, nullptr, stamp_out).asset;
}

CpuLoadResult AcquireCpuAssetAt(const std::string& path, const AssetFileStamp& now, AssetFileStamp* used)
{
    return Lookup(path, &now, used);
}

void EvictCpuAsset(const std::string& path)
{
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_entries.erase(path);
        g_evicted.Bump(path);  // a parse in flight now is stale (13-51)
    } catch (...) {
    }
}

void TrimCpuAssetCache(const std::vector<std::string>& wanted, bool check_files)
{
    try {
        std::vector<std::pair<std::string, FileStamp>> to_check;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_wanted.clear();
            g_wanted.insert(wanted.begin(), wanted.end());
            // Freeze fix #1: the other files are no longer all dropped here (each later use parsed
            // them again, on the UI thread): the recent ones stay, within the bounds.
            EnforceBoundsLocked();
            if (check_files)
                for (const auto& kv : g_entries) to_check.emplace_back(kv.first, kv.second.stamp);
        }
        if (to_check.empty()) return;
        std::vector<std::pair<std::string, FileStamp>> changed;
        for (const auto& e : to_check) {
            if (StampOf(e.first) != e.second) changed.push_back(e);
        }
        if (changed.empty()) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& c : changed) {
            const auto it = g_entries.find(c.first);
            // Only if it is still the version we checked (another thread may have just
            // reloaded it).
            if (it != g_entries.end() && it->second.stamp == c.second) g_entries.erase(it);
        }
    } catch (...) {
    }
}

void ClearCpuAssetCache()
{
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_entries.clear();
        g_wanted.clear();
    } catch (...) {
    }
}

}  // namespace rav

#endif  // _WIN32
