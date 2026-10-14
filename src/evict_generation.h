// SPDX-License-Identifier: MIT
//
// Eviction generations for the shared parse cache (13-51, issue #1 follow-up; asset_cache.cpp).
// "Locate textures..." evicts a file's parse because its textures changed, though the file's date
// and size did not. A parse the video thread started BEFORE that eviction still holds the
// texture-less model: it must not be stored back once it finishes. Each eviction bumps the
// path's generation; a parse remembers the generation it started at and is stored only if it is
// still the same. Pure, header-only, host-tested (tests/read_robustness_test.cpp); the caller
// holds its own lock around every call.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace rav {

class EvictGenerations {
public:
    // The path's generation now (0 = never evicted).
    uint64_t Of(const std::string& path) const
    {
        const auto it = gen_.find(path);
        return it == gen_.end() ? 0 : it->second;
    }
    // An eviction of `path`: parses started before it are stale.
    void Bump(const std::string& path) { ++gen_[path]; }

private:
    // Grows by one entry per evicted path (a Locate gesture), never per frame.
    std::unordered_map<std::string, uint64_t> gen_;
};

// True when a parse started at generation `at_start` may be stored now (no eviction since).
inline bool ParseStillCurrent(uint64_t at_start, uint64_t now)
{
    return at_start == now;
}

}  // namespace rav
