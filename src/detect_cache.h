// SPDX-License-Identifier: MIT
//
// The follow's detection cache (freeze fix #1, 2026-10-19). The follow (sound_follow.cpp) needs each
// animation item's detection (DetectItem: the file parsed, its bones sampled, the rules run). It used
// to run it again 5 s after the last one, for every item with sounds, on the UI thread: with the parse
// cache trimmed to the video FX's files, every one of them was a full parse again (minutes on 40 files).
//
// A detection depends only on what it reads, so it is kept for exactly that: the item's rules record
// (its text), its file (path and the version on disk: date and size) and the role mapping file (its
// text: the bones of the roles). No time limit: the same inputs give the same result, any change of
// one of them runs it again. Shared by the items that read the same inputs (copies of one item).
// Entries no item looked up in the last full pass are dropped.
//
// RunUnderBudget spreads the detections a tick has to run: at least one, then more while the tick's
// time budget lasts; the rest wait for the next ticks (the follow plans nothing until all are there,
// so what it writes is the same, only later).
//
// Pure, header-only, host-tested (tests/detect_cache_test.cpp). Main thread only (no lock).

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <tuple>
#include <utility>

namespace rav {

// What one detection reads.
struct DetectInputs {
    std::string        rules_raw;   // the item's rules record, as read
    std::string        path;        // its animation file
    long long          file_time = 0;  // the file's version on disk (AssetFileStamp)
    unsigned long long file_size = 0;
    bool operator<(const DetectInputs& o) const
    {
        return std::tie(rules_raw, path, file_time, file_size) < std::tie(o.rules_raw, o.path, o.file_time, o.file_size);
    }
    bool operator==(const DetectInputs& o) const
    {
        return rules_raw == o.rules_raw && path == o.path && file_time == o.file_time && file_size == o.file_size;
    }
};

template <class V>
class DetectCache {
public:
    // Starts a pass over the items. `roles_text`: the role mapping file now; another text than the
    // last pass's drops every entry (a role mapped to another bone changes the detections).
    void BeginPass(const std::string& roles_text)
    {
        if (!started_ || roles_text != roles_) entries_.clear();
        roles_ = roles_text;
        started_ = true;
        ++pass_;
    }
    // The detection of these inputs, null when none is kept. Marks it used in this pass.
    const V* Find(const DetectInputs& in)
    {
        const auto it = entries_.find(in);
        if (it == entries_.end()) return nullptr;
        it->second.pass = pass_;
        return &it->second.value;
    }
    bool Has(const DetectInputs& in) const { return entries_.count(in) != 0; }
    void Store(const DetectInputs& in, V value)
    {
        Slot& s = entries_[in];
        s.value = std::move(value);
        s.pass = pass_;
    }
    // After a full pass: the entries it did not look up are dropped.
    void DropUnused()
    {
        for (auto it = entries_.begin(); it != entries_.end();)
            it = it->second.pass == pass_ ? std::next(it) : entries_.erase(it);
    }
    void   Clear() { entries_.clear(); }
    size_t Size() const { return entries_.size(); }

private:
    struct Slot {
        V           value{};
        std::size_t pass = 0;
    };
    std::map<DetectInputs, Slot> entries_;
    std::string                  roles_;
    bool                         started_ = false;
    std::size_t                  pass_ = 0;
};

// Runs run(0), run(1)... run(n - 1) in order while elapsed_ms() (the time since the tick's work
// started) is under budget_ms; always at least one when n > 0. Returns how many ran.
template <class Run, class Elapsed>
std::size_t RunUnderBudget(std::size_t n, double budget_ms, Run run, Elapsed elapsed_ms)
{
    std::size_t done = 0;
    while (done < n) {
        if (done > 0 && !(elapsed_ms() < budget_ms)) break;
        run(done);
        ++done;
    }
    return done;
}

}  // namespace rav
