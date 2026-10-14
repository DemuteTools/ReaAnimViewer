// SPDX-License-Identifier: MIT
//
// When a cached read of a file must be done again (13-51, for 13-7b's peak cache): a file
// re-rendered at the same path, or one that could not be read and is now there. Pure,
// header-only, host-tested (tests/read_robustness_test.cpp).
//
// One version of a file = its date and size (both zero when it cannot be read). A cached entry
// keeps the version read BEFORE its file was read, so a write during the read is seen as a change
// afterwards. The disk is asked at most once per kFileRecheckSeconds per entry, never per frame.

#pragma once

namespace rav {

struct FileVersion {
    long long          time = 0;
    unsigned long long size = 0;
    bool operator==(const FileVersion& o) const { return time == o.time && size == o.size; }
    bool operator!=(const FileVersion& o) const { return !(*this == o); }
};

constexpr double kFileRecheckSeconds = 1.0;

// True when an entry last checked at `last_s` should ask the disk again at `now_s` (seconds on a
// steady clock). A clock that went back (now before last) checks too.
inline bool FileRecheckDue(double last_s, double now_s, double interval_s = kFileRecheckSeconds)
{
    return now_s < last_s || now_s - last_s >= interval_s;
}

// True when the cached read of a file is out of date: its version on disk is not the one read.
// A file still missing (zero then, zero now) keeps its failed entry until it appears.
inline bool FileReadStale(const FileVersion& read, const FileVersion& now)
{
    return read != now;
}

}  // namespace rav
