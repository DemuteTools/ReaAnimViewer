// SPDX-License-Identifier: MIT
//
// See sound_draw.h. Pure, deterministic on every platform (13-3, spec D9-D12).

#include "sound_draw.h"

#include "sound_entries.h"
#include "sound_zones.h"    // 13-62i: ZonesLaidOut

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

namespace {

// The last n (clamped as DrawPick: at most one less than the active count) of `history`.
bool InLastN(const std::vector<int>& history, int avoid_n, size_t active_count, int pick)
{
    const int    n = std::min(std::max(avoid_n, 0), static_cast<int>(active_count) - 1);
    const size_t from = history.size() > static_cast<size_t>(n) ? history.size() - static_cast<size_t>(n) : 0;
    return std::find(history.begin() + static_cast<std::ptrdiff_t>(from), history.end(), pick) != history.end();
}

bool Has(const std::vector<int>& v, int x)
{
    return std::find(v.begin(), v.end(), x) != v.end();
}

// What a draw picks among: `active` minus the last n of the history and the nearest k of `ahead`, k as large
// as leaves one (13-62i D: the history first, then the nearest picks after it); `active` when none is left.
std::vector<int> Candidates(const std::vector<int>& active, int avoid_n, const std::vector<int>& history,
                            const std::vector<int>& ahead)
{
    for (size_t k = ahead.size() + 1; k-- > 0;) {
        std::vector<int> cand;
        for (int a : active)
            if (!InLastN(history, avoid_n, active.size(), a) &&
                std::find(ahead.begin(), ahead.begin() + static_cast<std::ptrdiff_t>(k), a) == ahead.begin() + static_cast<std::ptrdiff_t>(k))
                cand.push_back(a);
        if (!cand.empty()) return cand;
    }
    return active;
}

}  // namespace

int DrawPick(const std::vector<int>& active, int avoid_n, const std::vector<int>& history, uint64_t seed,
             uint64_t salt, const std::vector<int>& ahead)
{
    if (active.empty()) return -1;
    const std::vector<int> cand = Candidates(active, avoid_n, history, ahead);
    return cand[static_cast<size_t>(Mix64(seed, salt) % cand.size())];
}

std::vector<int> ActiveVariations(const SoundData& data)
{
    std::vector<int> out;
    for (size_t i = 0; i < data.frames.size(); ++i)
        if (data.frames[i].on) out.push_back(static_cast<int>(i));
    return out;
}

std::vector<int> ActiveVariationsFor(const SoundData& data0, const std::string& part_value)
{
    std::vector<int> out;
    // 13-62h / 13-62i: a variation with at least one zone of that part (any part zone), every Sound (one not laid out
    // yet, laid out on a copy first).
    SoundData        tmp;
    const SoundData& data = ZonesLaidOut(data0, &tmp);
    for (int i : ActiveVariations(data)) {
        bool has = false;
        for (const EditZone& z : data.frames[static_cast<size_t>(i)].zones)
            has = has || (!z.IsGap() && (part_value.empty() || z.part == part_value));
        if (has) out.push_back(i);
    }
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
               uint64_t seed, uint64_t salt, const std::vector<int>& ahead)
{
    if (prev && Has(active, prev->pick)) {
        if (prev->hand) return prev->pick;
        // 13-62i (D, P3): a stored pick a draw would avoid (one of the last n of the history, or of the nearest picks
        // kept after it) is drawn again: a pick made under another history (a filtered Populate) still avoids repeating.
        if (Has(Candidates(active, avoid_n, history, ahead), prev->pick)) return prev->pick;
    }
    return DrawPick(active, avoid_n, history, seed, salt, ahead);
}

void Reshuffle(ItemSounds& item, uint64_t new_seed)
{
    item.seed = new_seed;
    for (EventSounds& ev : item.events)
        ev.draws.erase(std::remove_if(ev.draws.begin(), ev.draws.end(), [](const SoundDraw& d) { return !d.hand; }),
                       ev.draws.end());
}

}  // namespace rav
