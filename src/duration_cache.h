// SPDX-License-Identifier: MIT
//
// The animation sources' durations (freeze fix #2, 2026-10-19; pcm_source_anim.cpp). REAPER makes a
// new source for an animation item again and again (a project load, an undo, a copy, a glue...), and
// each one used to parse its file for the clip's length (ProbeAnimationDuration, an assimp read, on
// the thread REAPER calls it on). The length of one version of a file (its path, date and size) is
// read once per session: DurationCache. A version not seen whole (the stamp changed during the read)
// is not kept.
//
// StableStamp: a source re-reads its length when its file changed (a re-export), but only once the
// file stopped changing: the same new version read twice, at least the check interval apart (a file
// still being written is not parsed at each check).
//
// Pure, header-only, host-tested (tests/duration_cache_test.cpp). DurationCache is thread-safe.

#pragma once

#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace rav {

template <class Stamp>
class DurationCache {
public:
    explicit DurationCache(std::size_t max_files = 4096) : max_(max_files) {}

    // The length kept for this version of `path`.
    bool Find(const std::string& path, const Stamp& stamp, double* len) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = map_.find(path);
        if (it == map_.end() || !(it->second.first == stamp)) return false;
        if (len) *len = it->second.second;
        return true;
    }
    // One length per file: the latest version read replaces the previous one.
    void Store(const std::string& path, const Stamp& stamp, double len)
    {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (map_.size() >= max_ && map_.find(path) == map_.end()) map_.erase(map_.begin());
            map_[path] = std::make_pair(stamp, len);
        } catch (...) {
            // bad_alloc: not kept, read again next time
        }
    }
    std::size_t Size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return map_.size();
    }

private:
    mutable std::mutex                                 mutex_;
    std::map<std::string, std::pair<Stamp, double>>    map_;
    std::size_t                                        max_;
};

template <class Stamp>
class StableStamp {
public:
    // One check (the caller spaces them by its interval): `probed` = the version the length was read
    // from, `now` = the version on disk now. True = read the length again now (a new version, the
    // same as at the previous check).
    bool Check(const Stamp& probed, const Stamp& now)
    {
        if (now == probed) {
            pending_ = false;
            return false;
        }
        if (pending_ && seen_ == now) {
            pending_ = false;
            return true;
        }
        seen_ = now;
        pending_ = true;
        return false;
    }

private:
    Stamp seen_{};
    bool  pending_ = false;
};

}  // namespace rav
