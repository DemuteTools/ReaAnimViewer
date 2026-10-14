// SPDX-License-Identifier: MIT
//
// See reaper_item_read.h (13-51).

#include "reaper_item_read.h"

#include <cstring>
#include <memory>
#include <new>

namespace rav {

bool SameGuid(const std::string& a, const std::string& b)
{
    if (a.size() != b.size() || a.empty()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 32);
        if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 32);
        if (x != y) return false;
    }
    return true;
}

bool ReadGrowingText(const TextGetter& get, const std::vector<int>& sizes, std::string* out)
{
    if (out) out->clear();
    if (!get) return false;
    for (const int size : sizes) {
        if (size < 3) continue;
        // Not zero-filled: the getter writes the text and its terminator; the first byte is
        // cleared so a getter that writes nothing reads as "".
        std::unique_ptr<char[]> buf(new (std::nothrow) char[static_cast<size_t>(size)]);
        if (!buf) return false;  // out of memory: a bigger try would fail too
        buf[0] = '\0';
        if (!get(buf.get(), size)) continue;  // too small for some getters, or a failure: try bigger
        const void*  z = std::memchr(buf.get(), '\0', static_cast<size_t>(size));
        const size_t len = z ? static_cast<size_t>(static_cast<const char*>(z) - buf.get()) : static_cast<size_t>(size);
        if (len + 2 < static_cast<size_t>(size)) {
            if (out) out->assign(buf.get(), len);
            return true;
        }
        // Full: maybe cut, ask again bigger.
    }
    return false;
}

std::vector<int> DoublingSizes(int first, int cap)
{
    std::vector<int> sizes;
    if (first < 2 || cap < first) return sizes;
    for (long long s = first; s <= cap; s *= 2) sizes.push_back(static_cast<int>(s));
    return sizes;
}

}  // namespace rav

#ifdef _WIN32

namespace rav {

std::string ItemGuidOf(MediaItem* item)
{
    char buf[64] = {};
    if (!item || !GetSetMediaItemInfo_String(item, "GUID", buf, false)) return {};
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

MediaItem* ScanItemByGuid(ReaProject* proj, const std::string& guid)
{
    if (guid.empty()) return nullptr;
    const int n = CountMediaItems(proj);
    for (int i = 0; i < n; ++i) {
        MediaItem* it = GetMediaItem(proj, i);
        if (it && SameGuid(ItemGuidOf(it), guid)) return it;
    }
    return nullptr;
}

std::string ReadItemStateChunk(ItemStateChunkFn get_chunk, MediaItem* item)
{
    if (!get_chunk || !item) return {};
    static const std::vector<int> sizes = DoublingSizes(kItemChunkFirst, kItemChunkCap);
    std::string text;
    ReadGrowingText([&](char* buf, int size) { return get_chunk(item, buf, size, false); }, sizes, &text);
    return text;
}

}  // namespace rav

#endif  // _WIN32
