// SPDX-License-Identifier: MIT
//
// See library_media_plan.h.

#include "library_media_plan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
        sp.name += " " + std::to_string(pick.index + 1);
        d.mode = DetectMode::Silence;
        SoundFrame f = pick.frame;
        f.hand = true;
        d.frames = {f};
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

bool UseMediaAt(SoundStore& store, const std::string& lib_uid, const LibraryRow* row, const std::vector<MediaPick>& picks,
                std::vector<std::string>* created, std::string* toast, std::string* why)
{
    if (why) why->clear();
    const SoundLibrary* clib = FindLibrary(store, lib_uid);
    if (!clib) {
        if (why) *why = "Create a library first";
        return false;
    }
    if (picks.empty()) return false;
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

    SoundStore before = store;
    std::vector<std::string> made;
    std::string msg;
    bool ok = false;
    if (n && (n->type == SoundNodeType::Random || n->type == SoundNodeType::Blend)) {
        ok = AddNodes(store, lib_uid, &r, specs, &made, why);
        msg = Plural(k, "sound") + " added to the " + TypeWord(n->type);
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
    if (created) *created = sounds;
    if (toast) *toast = msg;
    if (why) why->clear();
    return true;
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

}  // namespace rav
