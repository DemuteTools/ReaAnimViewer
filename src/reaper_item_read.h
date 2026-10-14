// SPDX-License-Identifier: MIT
//
// Shared readers of REAPER item data (13-51): one GUID helper and one growing text reader, so the
// placement engine (13-6), Attach (13-10), Populate (13-8), the peak reader (13-7b) and the video
// FX state (11-3) cannot drift apart.
//
//  - SameGuid: REAPER writes GUIDs upper case, a record may keep another case: compared without
//    case. An empty GUID never matches.
//  - ReadGrowingText: REAPER getters that copy text into the caller's buffer either cut it (a full
//    buffer) or answer false when it is too small; we cannot tell that false from a real failure,
//    so both move on to the next, bigger size. The buffers are never zero-filled (only the first
//    byte is cleared), so a large try costs an allocation, not a 64 MB memset.
// The pure part (above the #ifdef) is host-tested (tests/reaper_item_read_test.cpp).

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace rav {

// True when both GUID texts are the same, ignoring ASCII case; false when either is empty.
bool SameGuid(const std::string& a, const std::string& b);

// One getter call: fills `buf` (`size` bytes, room for the terminator included); false = failed.
using TextGetter = std::function<bool(char* buf, int size)>;

// Calls `get` with each size in turn until the text fits: the getter said true and the text is
// shorter than size - 2 (a text that long may have been cut, so it is asked again bigger). True
// with the text in *out; false (and "" in *out) when the last size failed or was full.
bool ReadGrowingText(const TextGetter& get, const std::vector<int>& sizes, std::string* out);

// first, 2*first, ... up to cap (cap included when a power-of-two step reaches it). Empty when
// first < 2 or cap < first.
std::vector<int> DoublingSizes(int first, int cap);

// An item's state chunk: 64 KB doubled up to 64 MB (a big chunk, e.g. many takes or envelopes).
constexpr int kItemChunkFirst = 64 << 10;
constexpr int kItemChunkCap = 64 << 20;

}  // namespace rav

#if defined(_WIN32) && !defined(RAV_HOST_TEST)  // host tests on Windows: no REAPER SDK

#include "reaper_api.h"

namespace rav {

// The item's GUID text ("" when none).
std::string ItemGuidOf(MediaItem* item);

// Scans the project's items for this GUID (nullptr when none or guid is empty). proj nullptr =
// the current project.
MediaItem* ScanItemByGuid(ReaProject* proj, const std::string& guid);

// GetItemStateChunk's signature (resolved by each module through its get_func).
using ItemStateChunkFn = bool (*)(MediaItem* item, char* buf, int buf_sz, bool isundo);

// The item's state chunk ("" when it cannot be read), read through `get_chunk` with
// ReadGrowingText over DoublingSizes(kItemChunkFirst, kItemChunkCap).
std::string ReadItemStateChunk(ItemStateChunkFn get_chunk, MediaItem* item);

}  // namespace rav

#endif  // _WIN32 && !RAV_HOST_TEST
