// SPDX-License-Identifier: MIT
//
// See sound_draw.h. Pure, deterministic on every platform (13-3, spec D9-D12).

#include "sound_draw.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace rav {

namespace {

uint64_t SplitMix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

uint64_t Mix64(uint64_t seed, uint64_t salt)
{
    return SplitMix64(seed ^ SplitMix64(salt));
}

}  // namespace

int DrawPick(const std::vector<int>& active, int avoid_n, const std::vector<int>& history, uint64_t seed,
             uint64_t salt)
{
    if (active.empty()) return -1;
    const int max_n = static_cast<int>(active.size()) - 1;
    const int n = std::min(std::max(avoid_n, 0), max_n);
    const size_t from = history.size() > static_cast<size_t>(n) ? history.size() - static_cast<size_t>(n) : 0;
    std::vector<int> cand;
    for (int a : active)
        if (std::find(history.begin() + static_cast<std::ptrdiff_t>(from), history.end(), a) == history.end()) cand.push_back(a);
    if (cand.empty()) cand = active;
    return cand[static_cast<size_t>(Mix64(seed, salt) % cand.size())];
}

std::vector<int> ActiveVariations(const SoundData& data)
{
    std::vector<int> out;
    for (size_t i = 0; i < data.frames.size(); ++i)
        if (data.frames[i].on) out.push_back(static_cast<int>(i));
    return out;
}

uint64_t EventSalt(const std::string& rid, double t)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : rid) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    const long long bin = std::llround(t * 10.0);
    return Mix64(h, static_cast<uint64_t>(bin));
}

std::vector<const SoundNode*> ChildNodes(const SoundLibrary& lib, const SoundNode& node)
{
    std::vector<const SoundNode*> out;
    for (const SoundNode& n : lib.nodes)
        if (n.parent == node.uid && &n != &node) out.push_back(&n);
    return out;
}

std::vector<const SoundNode*> BlendParts(const SoundLibrary& lib, const SoundNode& node)
{
    if (node.type != SoundNodeType::Blend) return {};
    return ChildNodes(lib, node);
}

int KeepOrDraw(const SoundDraw* prev, const std::vector<int>& active, int avoid_n, const std::vector<int>& history,
               uint64_t seed, uint64_t salt)
{
    if (prev && std::find(active.begin(), active.end(), prev->pick) != active.end()) return prev->pick;
    return DrawPick(active, avoid_n, history, seed, salt);
}

void Reshuffle(ItemSounds& item, uint64_t new_seed)
{
    item.seed = new_seed;
    for (EventSounds& ev : item.events)
        ev.draws.erase(std::remove_if(ev.draws.begin(), ev.draws.end(), [](const SoundDraw& d) { return !d.hand; }),
                       ev.draws.end());
}

}  // namespace rav
