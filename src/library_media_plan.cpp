// SPDX-License-Identifier: MIT
//
// See library_media_plan.h.

#include "library_media_plan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "sound_entries.h"  // 13-67c: RefreshParts
#include "sound_record.h"

namespace rav {

namespace {

const char* const kSwitchRefused = "Pick a value row of this Switch first";

LibraryRow RootRow(const SoundLibrary& lib) { return {"", lib.root}; }

LibraryRow RowOrRoot(const SoundLibrary& lib, const LibraryRow* row)
{
    if (!row || row->key.empty()) return RootRow(lib);
    return *row;
}

// The name a slot is shown with: a value row's value name, else its node's name.
std::string SlotLabel(const SoundStore& store, const SoundLibrary& lib, const LibraryRow& r)
{
    if (RowIsValue(lib, r)) {
        const SoundNode* sw = FindNode(lib, r.parent);
        const SwitchGroup* g = sw ? FindGroup(store, sw->group_uid) : nullptr;
        if (g)
            for (const SwitchValue& v : g->values)
                if (v.uid == r.key) return v.name;
        return "the value";
    }
    const SoundNode* n = FindNode(lib, RowNode(lib, r));
    return n ? n->name : std::string();
}

const char* TypeWord(SoundNodeType t)
{
    switch (t) {
    case SoundNodeType::Random: return "Random";
    case SoundNodeType::Blend: return "Blend";
    case SoundNodeType::Switch: return "Switch";
    default: return "Sound";
    }
}

std::string Plural(int n, const char* word)
{
    return std::to_string(n) + " " + word + (n > 1 ? "s" : "");
}

}  // namespace

std::string MediaFileName(const std::string& path)
{
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string MediaFileStem(const std::string& path)
{
    std::string name = MediaFileName(path);
    size_t dot = name.find_last_of('.');
    return (dot == std::string::npos || dot == 0) ? name : name.substr(0, dot);
}

NewNodeSpec SoundSpecFor(const MediaPick& pick)
{
    NewNodeSpec sp;
    sp.type = SoundNodeType::Sound;
    sp.name = MediaFileStem(pick.path);
    SoundData& d = sp.sound;
    d.source.kind = SoundSourceKind::File;
    d.source.asset.name = MediaFileName(pick.path);
    d.source.asset.size = pick.size;
    d.source.asset.length = pick.length;
    d.source.asset.last_path = pick.path;
    d.source.asset.mtime = pick.mtime;
    if (pick.slice) {
        if (pick.slices.size() < 2) sp.name += " " + std::to_string(pick.index + 1);
        d.mode = DetectMode::Silence;
        d.frames = pick.slices.empty() ? std::vector<SoundFrame>{pick.frame} : pick.slices;
        for (SoundFrame& f : d.frames) f.hand = true;
        d.edited = true;
    } else if (!pick.frames.empty()) {
        d.mode = DetectMode::Silence;
        d.frames = pick.frames;
    } else {
        d.mode = DetectMode::Whole;
        if (pick.length > 0.0) {
            SoundFrame f;
            f.start = 0.0;
            f.length = pick.length;
            d.frames = {f};
        }
    }
    return sp;
}

std::vector<MediaPick> MergeVariationPicks(const std::vector<MediaPick>& picks)
{
    std::vector<MediaPick> out;
    out.reserve(picks.size());
    for (const MediaPick& p : picks) {
        MediaPick* into = nullptr;
        if (p.slice)
            for (MediaPick& o : out)
                if (o.slice && o.path == p.path) {
                    into = &o;
                    break;
                }
        if (!into) {
            out.push_back(p);
            continue;
        }
        if (into->slices.empty()) into->slices = {into->frame};
        const std::vector<SoundFrame> add = p.slices.empty() ? std::vector<SoundFrame>{p.frame} : p.slices;
        for (const SoundFrame& f : add) {
            bool dup = false;
            for (const SoundFrame& e : into->slices) dup = dup || (e.start == f.start && e.length == f.length);
            if (!dup) into->slices.push_back(f);
        }
        if (p.index < into->index) into->index = p.index;
    }
    for (MediaPick& o : out) {
        if (o.slices.size() < 2) continue;
        std::stable_sort(o.slices.begin(), o.slices.end(),
                         [](const SoundFrame& a, const SoundFrame& b) { return a.start < b.start; });
        o.frame = o.slices.front();
    }
    return out;
}

std::vector<MediaPick> MediaListDragPicks(const std::string& row_path, const std::vector<MediaPick>& files,
                                          const std::vector<MediaPick>& variations)
{
    if (variations.empty() || files.size() != 1 || files.front().path != row_path) return files;
    for (const MediaPick& v : variations)
        if (!v.slice || v.path != row_path) return files;
    return variations;
}

std::string MediaDragLabel(const std::vector<MediaPick>& picks)
{
    if (picks.empty()) return {};
    int n = 0;
    bool variations = false;
    for (const MediaPick& p : picks) {
        variations = variations || p.slice;
        n += p.slice && p.slices.size() >= 2 ? static_cast<int>(p.slices.size()) : 1;
    }
    if (n >= 2) return Plural(n, variations ? "variation" : "file");
    const MediaPick& p = picks.front();
    return p.slice ? MediaFileStem(p.path) + " " + std::to_string(p.index + 1) : MediaFileName(p.path);
}

bool MediaUseAddsInside(const SoundStore& store, const std::string& lib_uid, const LibraryRow* row)
{
    const SoundLibrary* lib = FindLibrary(store, lib_uid);
    if (!lib) return false;
    const SoundNode* n = FindNode(*lib, RowNode(*lib, RowOrRoot(*lib, row)));
    return n && (n->type == SoundNodeType::Random || n->type == SoundNodeType::Blend);
}

bool MediaUseLabel(const SoundStore& store, const std::string& lib_uid, const LibraryRow* row, int n_picks,
                   bool variations, std::string* label)
{
    if (label) label->clear();
    const SoundLibrary* lib = FindLibrary(store, lib_uid);
    if (!lib || n_picks <= 0) return false;
    LibraryRow r = RowOrRoot(*lib, row);
    const SoundNode* n = FindNode(*lib, RowNode(*lib, r));
    if (n && n->type == SoundNodeType::Switch) return false;
    std::string s;
    if (n && (n->type == SoundNodeType::Random || n->type == SoundNodeType::Blend))
        s = std::string("Add to ") + TypeWord(n->type);
    else
        s = "Use for " + SlotLabel(store, *lib, r);
    if (n_picks > 1)
        s += " (" + std::to_string(n_picks) + (variations ? " variations)" : " files)");
    else if (variations)
        s += " (1 variation)";
    if (label) *label = s;
    return true;
}

bool UseMediaAt(SoundStore& store, const std::string& lib_uid, const LibraryRow* row,
                const std::vector<MediaPick>& picks_in, std::vector<std::string>* created, std::string* toast,
                std::string* why, bool as_random, std::string* new_random, const MediaPeaksFn& peaks)
{
    if (new_random) new_random->clear();
    if (why) why->clear();
    const SoundLibrary* clib = FindLibrary(store, lib_uid);
    if (!clib) {
        if (why) *why = "Create a library first";
        return false;
    }
    if (picks_in.empty()) return false;
    // Antho 2026-10-19: one file's variations make one Sound of them; Alt (as_random) keeps a Sound per variation.
    const std::vector<MediaPick> picks = as_random ? picks_in : MergeVariationPicks(picks_in);
    const LibraryRow r = RowOrRoot(*clib, row);
    const SoundNode* n = FindNode(*clib, RowNode(*clib, r));
    if (n && n->type == SoundNodeType::Switch) {
        if (why) *why = kSwitchRefused;
        return false;
    }
    std::vector<NewNodeSpec> specs;
    specs.reserve(picks.size());
    for (const MediaPick& p : picks) specs.push_back(SoundSpecFor(p));
    const int k = static_cast<int>(specs.size());
    const std::string label = SlotLabel(store, *clib, r);
    // 13-69: the target's type, read now (`n` may not outlive the writes below).
    const SoundNodeType target_type = n ? n->type : SoundNodeType::Switch;

    SoundStore before = store;
    std::vector<std::string> made;
    std::string msg;
    bool ok = false;
    if (n && (n->type == SoundNodeType::Random || n->type == SoundNodeType::Blend)) {
        const SoundNodeType type = n->type;  // AddNodes changes `store`: `n` may not outlive it (seen under MSVC)
        ok = AddNodes(store, lib_uid, &r, specs, &made, why);
        msg = Plural(k, "sound") + " added to the " + TypeWord(type);
    } else if (n && n->type == SoundNodeType::Sound) {
        // Onto a sound: a new Random takes its slot, holding the old sound then the new ones.
        std::vector<std::string> wrapped;
        ok = WrapRows(store, lib_uid, {r}, SoundNodeType::Random, std::string(), &wrapped) && wrapped.size() == 1;
        if (ok) {
            const SoundLibrary* lib = FindLibrary(store, lib_uid);
            LibraryRow wr = RowOfNode(*lib, wrapped.front());
            ok = AddNodes(store, lib_uid, &wr, specs, &made, why);
        }
        msg = label + " is now a Random of " + std::to_string(k + 1) + " sounds (the old one + " + std::to_string(k) +
              " new)";
    } else {
        ok = AddNodes(store, lib_uid, &r, specs, &made, why);
        msg = k > 1 ? label + " is now a Random of " + std::to_string(k) + " sounds"
                    : label + " plays " + MediaFileName(picks.front().path);
    }
    if (!ok) {
        store = std::move(before);
        if (why && why->empty()) *why = "Nothing to add there";
        return false;
    }
    // The new Sounds (AddNodes may also report a holder Random), in line order.
    std::vector<std::string> sounds;
    const SoundLibrary* old_lib = FindLibrary(before, lib_uid);
    if (const SoundLibrary* lib = FindLibrary(store, lib_uid))
        for (const SoundNode& m : lib->nodes)
            if (m.type == SoundNodeType::Sound && !FindNode(*old_lib, m.uid)) sounds.push_back(m.uid);
    // 13-69: the new Sounds as ONE new Random (the holder the use made, else a group of them).
    if (as_random && sounds.size() >= 2) {
        const SoundLibrary* lib = FindLibrary(store, lib_uid);
        const SoundNode*    first = lib ? FindNode(*lib, sounds.front()) : nullptr;
        std::string         holder;
        if (first && !first->parent.empty() && !FindNode(*old_lib, first->parent)) {
            size_t kids = 0;
            for (const SoundNode& m : lib->nodes)
                if (m.parent == first->parent) ++kids;
            const SoundNode* hp = FindNode(*lib, first->parent);
            if (kids == sounds.size() && hp && hp->type == SoundNodeType::Random) holder = first->parent;
        }
        if (holder.empty()) {
            std::vector<LibraryRow> rows;
            for (const std::string& u : sounds) rows.push_back(RowOfNode(*lib, u));
            if (!GroupRows(store, lib_uid, rows, SoundNodeType::Random, std::string(), &holder)) holder.clear();
        }
        if (holder.empty()) {
            store = std::move(before);
            if (why) *why = "Could not make a Random there";
            return false;
        }
        // Named on the node itself: RenameRow on a value's content would rename the value.
        SoundNode* hn = FindNode(*FindLibrary(store, lib_uid), holder);
        const std::string stem = MediaCommonStem(picks);
        if (hn && !stem.empty()) hn->name = stem;
        const std::string hname = hn ? hn->name : std::string("Random");
        if (target_type == SoundNodeType::Random || target_type == SoundNodeType::Blend)
            msg = "New Random " + hname + " of " + Plural(k, "sound") + " in the " + TypeWord(target_type) + " " + label;
        else if (target_type == SoundNodeType::Sound)
            msg = label + " is now a Random of the old sound + a new Random " + hname + " of " + Plural(k, "sound");
        else
            msg = label + " is now a Random " + hname + " of " + Plural(k, "sound");
        if (new_random) *new_random = holder;
    }
    // 13-67c: zoned at creation (the Edit window draws zones: a Sound left with frames only showed none there).
    if (SoundLibrary* lib = FindLibrary(store, lib_uid))
        for (const std::string& u : sounds)
            if (SoundNode* sn = FindNode(*lib, u)) {
                PeakData   pk;
                const bool has = peaks && peaks(sn->sound.source.asset.last_path, &pk);
                RefreshParts(sn->sound, has ? &pk : nullptr);
            }
    if (k == 1 && picks.front().slices.size() >= 2)
        msg += " (" + std::to_string(picks.front().slices.size()) + " variations)";
    if (created) *created = sounds;
    if (toast) *toast = msg;
    if (why) why->clear();
    return true;
}

std::string MediaCommonStem(const std::vector<MediaPick>& picks)
{
    if (picks.empty()) return {};
    std::string pre = MediaFileStem(picks.front().path);
    auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    for (size_t i = 1; i < picks.size() && !pre.empty(); ++i) {
        const std::string s = MediaFileStem(picks[i].path);
        size_t k = 0;
        while (k < pre.size() && k < s.size() && lower(pre[k]) == lower(s[k])) ++k;
        pre.resize(k);
    }
    while (!pre.empty()) {
        const char c = pre.back();
        if (c == ' ' || c == '_' || c == '-' || c == '.' || c == '(' || (c >= '0' && c <= '9')) pre.pop_back();
        else break;
    }
    return pre.size() >= 2 ? pre : std::string();
}

std::string MediaDropHint(const SoundStore& store, const std::string& lib_uid, const LibraryRow* row, int n_picks,
                          bool variations, bool as_random)
{
    const SoundLibrary* lib = FindLibrary(store, lib_uid);
    if (!lib || n_picks <= 0) return {};
    const LibraryRow r = RowOrRoot(*lib, row);
    const SoundNode* n = FindNode(*lib, RowNode(*lib, r));
    if (n && n->type == SoundNodeType::Switch) return {};
    const std::string what = Plural(n_picks, variations ? "variation" : "file");
    const std::string label = SlotLabel(store, *lib, r);
    const bool        grouped = as_random && n_picks >= 2;
    const bool        one_sound = variations && n_picks >= 2 && !as_random;  // one file's variations: one Sound
    const int         sounds = one_sound ? 1 : n_picks;
    if (n && (n->type == SoundNodeType::Random || n->type == SoundNodeType::Blend))
        return what + (grouped ? " -> new Random in the " : one_sound ? " -> one sound added to the " : " -> added to the ") +
               TypeWord(n->type) + " " + label;
    if (n && n->type == SoundNodeType::Sound)
        return what + " -> " + label + " becomes a Random: the old sound + " +
               (grouped ? "a new Random of " + std::to_string(n_picks) : std::to_string(sounds) + " new");
    return what + " -> " + (sounds >= 2 ? "a new Random for " : one_sound ? "one sound plays for " : "plays for ") + label;
}

std::vector<size_t> MediaRangeRows(int anchor, int clicked, size_t n, size_t cap)
{
    std::vector<size_t> out;
    if (clicked < 0 || static_cast<size_t>(clicked) >= n || cap == 0) return out;
    if (anchor < 0 || static_cast<size_t>(anchor) >= n) anchor = clicked;
    size_t lo = static_cast<size_t>(std::min(anchor, clicked)), hi = static_cast<size_t>(std::max(anchor, clicked));
    if (hi - lo + 1 > cap) {  // the cap nearest the clicked row
        if (static_cast<size_t>(clicked) == hi) lo = hi + 1 - cap;
        else hi = lo + cap - 1;
    }
    for (size_t i = lo; i <= hi; ++i) out.push_back(i);
    return out;
}

std::string MediaSelectedLabel(size_t n)
{
    return n >= 2 ? std::to_string(n) + " selected" : std::string();
}

std::string FormatMediaFormat(const DbFile& f)
{
    std::string out;
    auto add = [&](const std::string& part) {
        if (!out.empty()) out += " \xC2\xB7 ";
        out += part;
    };
    if (f.channels > 0) add(std::to_string(f.channels) + "ch");
    if (f.bits > 0) add(std::to_string(f.bits) + "b");
    if (f.rate > 0) {
        char buf[32];
        if (f.rate % 1000 == 0)
            std::snprintf(buf, sizeof buf, "%dk", f.rate / 1000);
        else
            std::snprintf(buf, sizeof buf, "%.1fk", f.rate / 1000.0);
        add(buf);
    }
    return out;
}

std::string FormatMediaLength(double seconds)
{
    if (!(seconds >= 0.0) || !std::isfinite(seconds)) return {};
    double tenths = std::floor(seconds * 10.0 + 0.5);
    long long t = static_cast<long long>(tenths);
    long long m = t / 600;
    double s = (t % 600) / 10.0;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%lld:%04.1f", m, s);
    return buf;
}

int FrameAt(const std::vector<SoundFrame>& frames, double t)
{
    for (size_t i = 0; i < frames.size(); ++i)
        if (t >= frames[i].start && t < frames[i].start + frames[i].length) return static_cast<int>(i);
    return -1;
}

MediaBatch StartMediaBatch(std::vector<MediaPick> picks)
{
    MediaBatch b;
    b.picks = std::move(picks);
    b.read.reserve(b.picks.size());
    for (const MediaPick& p : b.picks) b.read.push_back(p.slice || p.frames_known ? PickRead::Ready : PickRead::Reading);
    return b;
}

std::vector<std::string> MediaBatchPathsToRead(const MediaBatch& b)
{
    std::vector<std::string> out;
    for (size_t i = 0; i < b.picks.size() && i < b.read.size(); ++i)
        if (b.read[i] == PickRead::Reading && std::find(out.begin(), out.end(), b.picks[i].path) == out.end())
            out.push_back(b.picks[i].path);
    return out;
}

void MediaBatchSetRead(MediaBatch& b, const std::string& path, bool ok, const std::vector<SoundFrame>& frames,
                       double length)
{
    for (size_t i = 0; i < b.picks.size() && i < b.read.size(); ++i) {
        MediaPick& p = b.picks[i];
        if (b.read[i] != PickRead::Reading || p.path != path) continue;
        b.read[i] = ok ? PickRead::Ready : PickRead::Failed;
        p.frames = ok ? frames : std::vector<SoundFrame>{};
        p.frames_known = true;
        if (ok && length > 0.0) p.length = length;
    }
}

void MediaBatchSetMissing(MediaBatch& b, const std::string& path)
{
    for (size_t i = 0; i < b.picks.size() && i < b.read.size(); ++i)
        if (b.read[i] == PickRead::Reading && b.picks[i].path == path) b.read[i] = PickRead::Missing;
}

std::vector<MediaPick> MediaBatchUsedPicks(const MediaBatch& b)
{
    std::vector<MediaPick> out;
    for (size_t i = 0; i < b.picks.size(); ++i)
        if (i >= b.read.size() || b.read[i] != PickRead::Missing) out.push_back(b.picks[i]);
    return out;
}

std::vector<std::string> MediaBatchMissingNames(const MediaBatch& b)
{
    std::vector<std::string> out;
    for (size_t i = 0; i < b.picks.size() && i < b.read.size(); ++i) {
        if (b.read[i] != PickRead::Missing) continue;
        std::string n = MediaFileName(b.picks[i].path);
        if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    }
    return out;
}

int MediaBatchLeft(const MediaBatch& b) { return static_cast<int>(MediaBatchPathsToRead(b).size()); }

std::vector<std::string> MediaBatchFailedNames(const MediaBatch& b)
{
    std::vector<std::string> out;
    for (size_t i = 0; i < b.picks.size() && i < b.read.size(); ++i) {
        if (b.read[i] != PickRead::Failed) continue;
        std::string n = MediaFileName(b.picks[i].path);
        if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    }
    return out;
}

std::string MediaReadingLabel(int n_files) { return "Reading " + Plural(n_files, "file") + "..."; }

std::string MediaFailedToastSuffix(const std::vector<std::string>& names)
{
    if (names.empty()) return {};
    std::string s = "; could not read ";
    for (size_t i = 0; i < names.size(); ++i) s += (i ? ", " : "") + names[i];
    return s + ": added whole";
}

std::string MediaMissingToastSuffix(const std::vector<std::string>& names)
{
    if (names.empty()) return {};
    std::string s = "; missing ";
    for (size_t i = 0; i < names.size(); ++i) s += (i ? ", " : "") + names[i];
    return s + ": not added";
}

std::vector<const DbFile*> MediaListAll(const std::vector<DbFile>& files, const std::string& db_filter)
{
    std::vector<const DbFile*> out;
    out.reserve(db_filter.empty() ? files.size() : 0);
    for (const DbFile& f : files)
        if (db_filter.empty() || f.db == db_filter) out.push_back(&f);
    return out;
}

// ---- MediaExistsCache --------------------------------------------------------------------------

MediaExistsCache::State MediaExistsCache::Get(const std::string& path) const
{
    auto it = state_.find(path);
    return it == state_.end() ? State::Unknown : it->second;
}

void MediaExistsCache::Want(const std::string& path)
{
    if (state_.count(path)) return;
    if (std::find(want_.begin(), want_.end(), path) == want_.end()) want_.push_back(path);
}

std::vector<std::string> MediaExistsCache::EndFrame(size_t max, uint64_t* gen)
{
    std::vector<std::string> batch;
    if (!out_ && max > 0) {
        for (const std::string& p : want_) {
            if (batch.size() >= max) break;
            batch.push_back(p);
            state_[p] = State::Checking;
        }
        if (!batch.empty()) {
            out_ = true;
            if (gen) *gen = gen_;
        }
    }
    want_.clear();
    return batch;
}

void MediaExistsCache::Answer(uint64_t gen, const std::vector<std::string>& paths, const std::vector<char>& found)
{
    if (gen != gen_) return;
    out_ = false;
    for (size_t i = 0; i < paths.size(); ++i) {
        auto it = state_.find(paths[i]);
        // A path known some other way meanwhile keeps that answer (it is the newer one).
        if (it == state_.end() || it->second == State::Checking)
            state_[paths[i]] = i < found.size() && found[i] ? State::Found : State::Missing;
    }
}

void MediaExistsCache::Set(const std::string& path, bool found) { state_[path] = found ? State::Found : State::Missing; }

void MediaExistsCache::Clear()
{
    state_.clear();
    want_.clear();
    ++gen_;
    out_ = false;
}

}  // namespace rav
