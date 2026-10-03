// SPDX-License-Identifier: MIT
//
// See asset_cache.h.

#include "asset_cache.h"

#ifdef _WIN32

#include <chrono>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "console_log.h"

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
};

std::mutex                              g_mutex;
std::unordered_map<std::string, Entry>  g_entries;  // guarded by g_mutex
std::unordered_set<std::string>         g_wanted;   // guarded by g_mutex

}  // namespace

AssetFileStamp ReadAssetFileStamp(const std::string& path)
{
    return StampOf(path);
}

std::shared_ptr<const CpuAsset> AcquireCpuAsset(const std::string& path)
{
    if (path.empty()) return nullptr;
    try {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const auto it = g_entries.find(path);
            if (it != g_entries.end()) return it->second.asset;
        }
        // Miss: parse here, outside the lock (seconds for a big FBX), so other threads'
        // lookups never wait on it. If two threads miss the same file at once both parse
        // it; the last one stored wins, both results are valid.
        const FileStamp stamp = StampOf(path);
        const auto t0 = std::chrono::steady_clock::now();
        CpuLoadResult r = LoadCpuAsset(path);
        // The parse blocks the calling thread (REAPER's video thread on a miss): logged so
        // the debuglog gate sees that stall next to the per-frame cost lines.
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!r.asset) {
            LogError("video FX: load failed [%s] %s: %s", LoadErrorCategoryName(r.category),
                     path.c_str(), r.detail.c_str());
        } else {
            LogInfo("video FX: parsed %s in %.0f ms", path.c_str(), ms);
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_entries[path] = Entry{r.asset, stamp};
        return r.asset;
    } catch (...) {
        return nullptr;  // bad_alloc on the map: no picture, try again next frame
    }
}

void OfferCpuAsset(const std::string& path, std::shared_ptr<const CpuAsset> asset,
                   const AssetFileStamp& stamp)
{
    if (path.empty() || !asset) return;
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_wanted.find(path) == g_wanted.end()) return;
        Entry& e = g_entries[path];
        // Same version already parsed: keep it (a new parse object would make the video
        // thread drop and re-upload its GPU copy of an unchanged file).
        if (e.asset && e.stamp == stamp) return;
        // The stamp was read before the viewer's parse: if the file changed during it,
        // the 1 Hz check sees the difference and the file is read again.
        e = Entry{std::move(asset), stamp};
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
            for (auto it = g_entries.begin(); it != g_entries.end();) {
                if (g_wanted.find(it->first) == g_wanted.end()) {
                    it = g_entries.erase(it);
                } else {
                    if (check_files) to_check.emplace_back(it->first, it->second.stamp);
                    ++it;
                }
            }
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
