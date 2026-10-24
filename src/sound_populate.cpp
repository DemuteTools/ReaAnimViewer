// SPDX-License-Identifier: MIT
//
// See sound_populate.h. Populate = resolve (13-4) -> draw (13-3) -> place (13-6), recorded in the
// item's RAV_SOUNDS; the decisions are sound_populate_plan.h's.

#include "sound_populate.h"

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "asset_cache.h"
#include "asset_loader.h"
#include "busy_progress_ui.h"  // 13-59: the busy card of a long gesture
#include "event_list.h"
#include "item_rules.h"
#include "library_user_ui.h"  // 13-11: File Sounds are placed straight from their file
#include "ravlib.h"
#include "rule_record.h"
#include "sound_assign.h"
#include "sound_auto_assign.h"
#include "sound_draw.h"
#include "sound_entries.h"
#include "sound_follow_plan.h"
#include "sound_peaks.h"  // 13-25: sync points measured at placement
#include "sound_place.h"
#include "sound_place_plan.h"
#include "sound_populate_plan.h"
#include "sound_project.h"
#include "sound_record.h"
#include "sound_spread_plan.h"  // 13-68e: FillMissingSideValues
#include "sound_switch.h"
#include "sound_sync_plan.h"  // 13-35: Sync placed sounds
#include "sound_tracks.h"
#include "tagging_session.h"
#include "tagging_signal.h"

namespace rav {

namespace av = assign;  // 13-66: the staged entries Commit is given (assign_view_model.h)

namespace {

using SetMediaItemSelectedFn = void (*)(MediaItem* item, bool selected);
SetMediaItemSelectedFn f_set_selected = nullptr;
using SetItemValueFn = bool (*)(MediaItem* item, const char* parmname, double newvalue);
SetItemValueFn f_set_item_value = nullptr;  // 13-62i (D): a kept item's D_VOL

constexpr int kUnselectAllItems = 40289;  // Item: Unselect (clear selection of) all items

// 13-59 review: the report line of an item whose sound record would pass kItemSoundsMaxBytes.
constexpr const char* kRecordTooLargeText =
    "Not saved: this item's sound record is too large (too many loop passes or sounds); its new sounds are not tracked";

// The project scan behind FindItemByGuid.
MediaItem* FindPlacedItem(const std::string& guid) { return ScanItemByGuid(nullptr, guid); }

// One gesture's GUID -> item map (built once, kept in step with the items the gesture places and
// removes, dropped when the gesture ends: never a pointer kept across gestures).
std::string GuidKey(const std::string& g)
{
    std::string k = g;
    for (char& c : k)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    return k;
}

std::map<std::string, MediaItem*>* g_items = nullptr;
// 13-25 (D6): a Sound whose stored sync points are stale (never measured, or measured under other
// settings) is measured at placement, once a gesture: library uid + "/" + Sound uid -> its data with
// those syncs, and the variations measured from real peaks (written back to the store when the
// gesture ends, in its undo point: WriteMeasuredSyncs).
struct MeasuredSound {
    SoundData                               data;
    std::set<int>                           measured;
    std::set<std::pair<int, std::string>>   tried;  // (variation, file) measured in vain: not again
};
// The gesture's measures: the Sounds, each file's peaks read once (null = none), and the time the
// gesture may still wait for peaks (RequestPeaks reads on its thread: a cold file takes a while).
struct MeasureCache {
    std::map<std::string, MeasuredSound>             sounds;
    std::map<std::string, std::unique_ptr<PeakData>> peaks;
    std::map<std::string, FileVersion>               versions;  // 13-54 review: each file's date + size, read once
    std::map<std::string, std::string>               files;     // 13-54 review: each margin Sound's file, asked once
    bool                                             started = false;
    std::chrono::steady_clock::time_point            deadline;
};
MeasureCache* g_measured = nullptr;
void WriteMeasuredSyncs(const std::map<std::string, MeasuredSound>& measured);
// 13-13: the gesture's Switch-named tracks, track path -> track (null = could not be made: said
// once, the sounds on their usual tracks). Found or made once per gesture (SwitchTrackFor).
std::map<std::string, MediaTrack*>* g_tracks = nullptr;

// The engine's finder: the gesture's map only (a miss, or a GUID known as none: the engine scans).
// Resilience 2026-10-19: the gesture's Populated copies in the project by owner (GUID upper case), read from their tags once a
// gesture (built at first use; null outside a gesture: read each time).
std::map<std::string, std::vector<StrayCopy>>* g_tagged = nullptr;
bool                                           g_tagged_built = false;

MediaItem* IndexedItem(const std::string& guid)
{
    if (!g_items) return nullptr;
    auto hit = g_items->find(GuidKey(guid));
    return hit == g_items->end() ? nullptr : hit->second;
}

class ItemIndexScope {
public:
    ItemIndexScope()
    {
        if (g_items) return;  // nested: the outer gesture's map
        const int n = CountMediaItems(nullptr);
        for (int i = 0; i < n; ++i)
            if (MediaItem* it = GetMediaItem(nullptr, i)) {
                char buf[64] = {};
                if (GetSetMediaItemInfo_String(it, "GUID", buf, false) && buf[0]) map_[GuidKey(buf)] = it;
            }
        g_items = &map_;
        g_tracks = &tracks_;
        g_tagged = &tagged_;  // Resilience 2026-10-19
        g_tagged_built = false;
        g_measured = &measured_;
        owner_ = true;
        SetPlaceItemFinder(&IndexedItem);
        PlaceBatch(true);  // Antho 2026-10-12: one redraw for the whole gesture, each source chunk read once
    }
    ~ItemIndexScope()
    {
        if (!owner_) return;
        try {
            WriteMeasuredSyncs(measured_.sounds);  // 13-25: in the gesture's undo point
        } catch (...) {
        }
        g_measured = nullptr;
        PlaceBatch(false);
        SetPlaceItemFinder(nullptr);
        g_items = nullptr;
        g_tracks = nullptr;
        g_tagged = nullptr;
        g_tagged_built = false;
    }
    ItemIndexScope(const ItemIndexScope&) = delete;
    ItemIndexScope& operator=(const ItemIndexScope&) = delete;

private:
    std::map<std::string, MediaItem*>  map_;
    std::map<std::string, MediaTrack*> tracks_;
    MeasureCache                       measured_;
    std::map<std::string, std::vector<StrayCopy>> tagged_;
    bool                               owner_ = false;
};

// RemovePlacement, the item dropped from the gesture's map.
bool RemoveIndexed(const std::string& g)
{
    if (g_items) g_items->erase(GuidKey(g));
    return RemovePlacement(nullptr, g);
}

// Resilience 2026-10-19: the Populated copies tagged for `owner` still in the project (any record may list them: the caller
// sorts them out, AdoptStrayCopies).
std::vector<StrayCopy> PopulatedCopiesOf(const std::string& owner)
{
    std::map<std::string, std::vector<StrayCopy>>  local;
    std::map<std::string, std::vector<StrayCopy>>* m = g_tagged ? g_tagged : &local;
    if (!g_tagged || !g_tagged_built) {
        const int n = CountMediaItems(nullptr);
        for (int i = 0; i < n; ++i) {
            MediaItem* it = GetMediaItem(nullptr, i);
            PlacedTag  tag;
            if (!it || !ReadPlacedTag(it, &tag) || tag.own != Ownership::Populated || tag.owner.empty()) continue;
            StrayCopy c{ItemGuidOf(it), tag.rid, tag.t, tag.node};
            c.has_at = PlacedPileAnchor(it, &c.anchor, &c.sync);  // review 2026-10-19: where it sits
            (*m)[GuidKey(tag.owner)].push_back(c);
        }
        if (g_tagged) g_tagged_built = true;
    }
    std::vector<StrayCopy> out;
    const auto             hit = m->find(GuidKey(owner));
    if (hit != m->end())
        for (const StrayCopy& c : hit->second)
            if (FindItemByGuid(c.item_guid)) out.push_back(c);  // not removed meanwhile by the gesture
    return out;
}

// Resilience 2026-10-19: the GUIDs of the project's tracks (upper case).
std::set<std::string> ProjectTrackGuids()
{
    std::set<std::string> out;
    for (int i = 0, n = CountTracks(nullptr); i < n; ++i) {
        MediaTrack* tr = GetTrack(nullptr, i);
        char        buf[64] = {};
        if (tr && GetSetMediaTrackInfo_String(tr, "GUID", buf, false) && buf[0]) out.insert(GuidKey(buf));
    }
    return out;
}

uint64_t RandomSeed()
{
    std::random_device rd;
    uint64_t s = (static_cast<uint64_t>(rd()) << 32) ^ rd();
    return s ? s : 0x9E3779B97F4A7C15ull;
}

// What one pass over an item does.
struct PopOpts {
    // Antho 2026-10-12 (Populate "did nothing"): what a Populate did, for its report line. Optional.
    PopulateCounts* counts = nullptr;
    // Antho 2026-10-12: the Populate button places again the sounds deleted by hand (a click asks for them);
    // the automatic re-placements (a setting changed, the follow) still leave them deleted.
    bool        revive_deleted = false;
    bool        only_populated = false;  // P8: only the events that already have a live placement
    // Resilience 2026-10-19: an assignment change: on an item that has live populated sounds, every event of the rules follows it
    // (PopulateFilter::follow_item), not only the populated ones.
    bool        follow_assign = false;
    bool        report_replaced = true;
    // 13-56 review: a looped item's new passes filled by the follow (PopulateLoopPassesNoUndo): only
    // the Place steps of the events that already have their sounds (PlanPassFill); nothing removed,
    // replaced or re-drawn, no orphan prune.
    bool        fill_passes = false;
    // 13-35: a Sync of placed sounds (SyncPlacedSounds; null = a Populate). Only the populated sounds behind on a node of
    // its scope are touched, in place, keeping the draw (a variation switched off is drawn again); nothing is placed,
    // removed or replaced by another node, and nothing is pruned.
    const struct SyncRun* sync = nullptr;
    std::string rid;                      // "" = every rule
    std::vector<std::string> rids;        // 13-8 fix: when not empty, only these rules (one detection for all)
    bool        has_t = false;
    double      t = 0.0;
    // 13-19: told before each of the item's events (handled so far, the item's count). Optional.
    std::function<void(int, int)> on_event;
    // 13-68 (Antho 2026-10-18): the draws carried from item to item of one gesture, items in time order, so
    // avoid-repeat holds across separate animation items (three Jump clips, one FS L each, drew 2, 2, 1). Optional.
    std::map<std::string, std::vector<int>>* carry = nullptr;
};

// The items in time order (13-68: the order the carried draws follow).
std::vector<MediaItem*> ByPosition(std::vector<MediaItem*> items)
{
    std::stable_sort(items.begin(), items.end(), [](MediaItem* a, MediaItem* b) {
        return GetMediaItemInfo_Value(a, "D_POSITION") < GetMediaItemInfo_Value(b, "D_POSITION");
    });
    return items;
}

std::string NodeName(const SoundNode* n)
{
    if (!n) return "?";
    return n->name.empty() ? n->uid : n->name;
}

void AddLine(SoundReport& rep, ReportKind kind, const std::string& text, const std::string& node,
             const std::string& item_guid, double t)
{
    ReportLine l;
    l.kind = kind;
    l.text = text;
    l.node = node;
    l.item_guid = item_guid;
    l.t = t;
    rep.lines.push_back(l);
}

// 13-13: the track at the end of a track path (SoundTrackPath; nullptr when empty), found or
// made once per gesture (g_tracks). Call only once the sound's source resolved (no empty track for
// a missing file or item). A track that cannot be made is reported once a gesture.
MediaTrack* SwitchTrackFor(const std::vector<TrackStep>& path, SoundReport& rep, const std::string& node,
                           const std::string& item_guid, double t)
{
    if (path.empty()) return nullptr;
    std::string key;
    for (const TrackStep& st : path) {  // 13-16: the marks too (two Blends may name their layers alike)
        key += st.name;
        key += '\x1e';
        key += st.mark;
        key += '\x1f';
    }
    std::map<std::string, MediaTrack*>  local;  // outside a gesture (never today): this call only
    std::map<std::string, MediaTrack*>& cache = g_tracks ? *g_tracks : local;
    auto                                hit = cache.find(key);
    if (hit != cache.end()) return hit->second;
    std::string failed;
    MediaTrack* tr = EnsureTrackPath(nullptr, path, &failed);
    if (!tr)
        AddLine(rep, ReportKind::Missing, "Could not make track " + failed, node,
                item_guid, t);
    cache.emplace(key, tr);
    return tr;
}

// P13: every placed item's tag names the animation item.
void CheckOwners(const std::vector<std::string>& guids, const std::string& owner, SoundReport& rep,
                 const std::string& node, double t)
{
    for (const std::string& g : guids) {
        PlacedTag tag;
        MediaItem* it = FindItemByGuid(g);
        if (!it || !ReadPlacedTag(it, &tag) || !SameGuid(tag.owner, owner))
            AddLine(rep, ReportKind::Blocked, "tag owner mismatch", node, owner, t);
    }
}

// 13-25: a whole file's peaks for measuring sync points at placement, read once a gesture. RequestPeaks
// reads a file on its reader thread: it is called until the peaks are ready, at most ~3 s a file and ~10 s a
// gesture (else none: the sync falls back to the first span's start + the offset, PieceApplyFor, and is
// measured by a later gesture). Null when there are none. 13-57: a free reader starts it at once (reads
// run side by side); when every reader is busy, a read nobody asks for any more is given up about half a
// second in and this file is read (RequestPeaks, PickPeakCancel).
const PeakData* WholeFilePeaks(MeasureCache& cache, const std::string& path)
{
    if (path.empty()) return nullptr;
    auto hit = cache.peaks.find(path);
    if (hit != cache.peaks.end()) return hit->second.get();
    const auto now = std::chrono::steady_clock::now();
    if (!cache.started) {
        cache.started = true;
        cache.deadline = now + std::chrono::seconds(10);
    }
    const auto until = std::min(now + std::chrono::seconds(3), cache.deadline);
    auto       pk = std::make_unique<PeakData>();
    for (;;) {
        const PeakState st = RequestPeaks(path, pk.get());
        if (st == PeakState::Ready) break;
        if (st == PeakState::Failed || std::chrono::steady_clock::now() > until) {
            pk.reset();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));  // 13-55: read on the reader thread
    }
    return cache.peaks.emplace(path, std::move(pk)).first->second.get();
}

// 13-25 (D6): the Sound's data a placement of variation `v` reads: its stored data, or, when the
// variation's sync points are stale, a copy with them measured over the file's peaks (once a gesture,
// g_measured; `local` outside a gesture). `path_of` gives the file it plays (asked only to measure;
// 13-54 review: asked once a gesture for a Sound with part margins, whose trims are checked against the
// file's date + size too). A variation measured in vain from a file is not measured again from it in the gesture.
const SoundData& PlacedDataFor(const std::string& lib_uid, const SoundNode& n, int v,
                               const std::function<std::string()>& path_of, MeasureCache& local)
{
    if (v < 0 || v >= static_cast<int>(n.sound.frames.size()) || n.sound.mode == DetectMode::Takes) return n.sound;
    MeasureCache& cache = g_measured ? *g_measured : local;
    // 13-54 review: a divided Silence Sound's part trims are also stale when measured on another version
    // of its file (re-rendered at the same path): its date + size asked once per gesture.
    const bool        margins = PartMarginsApply(n.sound);
    const std::string key = lib_uid + "/" + n.uid;
    std::string       file;
    FileVersion       version;
    if (margins) {
        auto fh = cache.files.find(key);
        if (fh == cache.files.end()) fh = cache.files.emplace(key, path_of()).first;
        file = fh->second;
        if (!file.empty()) {
            auto vh = cache.versions.find(file);
            if (vh == cache.versions.end()) vh = cache.versions.emplace(file, SourceFileVersion(file)).first;
            version = vh->second;
        }
    }
    auto stale = [&](const SoundData& d) { return SyncsStale(d, v) || (margins && PartTrimsStale(d, v, &version)); };
    if (!stale(n.sound)) return n.sound;
    auto hit = cache.sounds.find(key);
    if (hit == cache.sounds.end()) hit = cache.sounds.emplace(key, MeasuredSound{n.sound, {}, {}}).first;
    MeasuredSound& m = hit->second;
    if (!stale(m.data)) return m.data;
    // 13-54: the part margins are measured on the peaks too, whatever the sync mode.
    const std::string path = margins ? file : n.sound.sync_mode == SyncMode::SliceStart ? std::string() : path_of();
    if (!m.tried.insert({v, path}).second) return m.data;
    const PeakData* peaks = path.empty() ? nullptr : WholeFilePeaks(cache, path);
    if (MeasureSyncs(m.data, v, peaks)) m.measured.insert(v);
    return m.data;
}

// 13-62i: the file variation `v` of a Sound plays (Takes mode: its take's), "" when not found.
std::string PlacedSourcePath(const SoundNode& n, int v)
{
    if (n.sound.mode != DetectMode::Takes) return SoundSourcePath(n.sound.source);
    SoundMedia m;
    return ReadSoundMedia(n.sound.source, &m) ? VariationPath(m, n.sound, v) : std::string();
}

// 13-62h: the peaks a zoned placement's snap reads (ZoneSnap on its anchor zone: Attack / Peak without a snap of
// its own, PieceSnapNeedsPeaks), read once a gesture like the syncs above; null when it needs none or none could
// be read (the zone's detected onset, else its start). Nothing is detected here (D6): the zones are as stored.
const PeakData* ZoneSnapPeaks(const SoundNode& n, int v, const std::string& part, const std::function<std::string()>& path_of,
                              MeasureCache& local)
{
    if (!PieceSnapNeedsPeaks(n.sound, v, part)) return nullptr;
    MeasureCache&     cache = g_measured ? *g_measured : local;
    const std::string path = path_of();
    return path.empty() ? nullptr : WholeFilePeaks(cache, path);
}

void WriteMeasuredSyncs(const std::map<std::string, MeasuredSound>& measured)
{
    bool any = false;
    for (const auto& kv : measured) any = any || !kv.second.measured.empty();
    if (!any) return;
    ModifySoundStore(nullptr, nullptr, [&](SoundStore& st) {
        bool changed = false;
        for (const auto& kv : measured) {
            const size_t slash = kv.first.find('/');
            if (slash == std::string::npos || kv.second.measured.empty()) continue;
            changed = ApplyMeasuredSyncs(st, kv.first.substr(0, slash), kv.first.substr(slash + 1), kv.second.data,
                                         kv.second.measured) || changed;
        }
        return changed;
    });
}

// 13-23: a value's name in the Sound's divide group ("" for none).
std::string DivideValueName(const SoundStore& store, const SoundData& data, const std::string& value)
{
    if (value.empty()) return {};
    if (const SwitchGroup* g = FindGroup(store, data.divide_group))
        for (const SwitchValue& v : g->values)
            if (v.uid == value) return v.name.empty() ? std::string("(unnamed value)") : v.name;
    return "(deleted value)";
}

// Spec missing-sounds-window: an item Sound whose item is gone, the one predicate of a gesture: the gesture's item map
// first (cheap, the common case: its item is there), then ItemSoundGone, which PrepareFileSound and the missing rule
// read, so they never disagree on a Sound this says is gone.
bool ItemGoneHere(const SoundNode& n)
{
    return n.sound.source.kind == SoundSourceKind::Item && !FindItemByGuid(n.sound.source.item_guid) && ItemSoundGone(n);
}

// A File Sound, or an item Sound whose item is gone (it plays the file it remembers).
bool PlaysFromFile(const SoundNode& n)
{
    return n.sound.source.kind == SoundSourceKind::File || ItemGoneHere(n);
}

// The request for one resolved sound (P11). `file` = a File Sound's target (PrepareFileSound). 13-20 /
// 13-23 / 13-25: `data` = the Sound's data to place (its syncs measured, PlacedDataFor; null = its
// own): a sliced, Whole-item or File variation is placed as the pieces of its part (PieceApplyFor). 13-62h:
// `zone_peaks` = what a zoned anchor's snap reads (ZoneSnapPeaks).
PlaceRequest RequestFor(MediaItem* owner, const std::string& rid, double event_t, double project_t,
                        const SoundNode& n, const ResolvedSound& rs, const FileSoundTarget* file = nullptr,
                        const SoundData* data = nullptr, const PeakData* zone_peaks = nullptr)
{
    const SoundData& sd = data ? *data : n.sound;
    PlaceRequest req;
    req.owner = owner;
    req.rid = rid;
    req.event_t = event_t;
    req.project_t = project_t;
    req.node = n.uid;
    req.own = Ownership::Populated;
    req.source = n.sound.source;
    const int v = rs.variation;
    if (file && !file->path.empty()) {
        // Antho 2026-10-12 (13-11): straight from the file, on its Lands-in layer track. Spec missing-sounds-window:
        // an item Sound whose item is gone too, from the file it remembers.
        req.source.kind = SoundSourceKind::File;
        req.file_path = file->path;
        req.track = file->track;
        req.slice = FileSliceFor(sd, v);
        req.fades = FileFadeApplyFor(sd, v);
        req.pieces = PieceApplyFor(sd, v, rs.part_value, true, zone_peaks);
    } else if (n.sound.source.kind == SoundSourceKind::File && file && !file->item_guid.empty()) {
        req.source.kind = SoundSourceKind::Item;  // an older build's imported item: copied as before
        req.source.item_guid = file->item_guid;
    }
    if (req.source.kind == SoundSourceKind::Item && v >= 0 && v < static_cast<int>(n.sound.frames.size())) {
        if (sd.mode == DetectMode::Takes) req.variation = v;  // 13-62i: its take, made active before its pieces
        if (sd.mode == DetectMode::TakeMarkers || sd.mode == DetectMode::Silence) req.pile_slice = v;  // 13-68 v2: a folder pile's members, slice v
        req.pile_silence = sd.mode == DetectMode::Silence;  // Antho 2026-10-20: each member's own k-th region of sound
        req.pile_params = sd.silence;
        if (sd.mode == DetectMode::TakeMarkers || sd.mode == DetectMode::Silence)
            req.slice = SliceApplyFor(sd.frames[static_cast<size_t>(v)]);
        req.fades = FadeApplyFor(sd, v);  // Antho 2026-10-12: fades on every placed variation
        // 13-20 / 13-23 / 13-25 (D7): the part's pieces, each from the sync point (a folder pile ignores them:
        // PlaceSounds). 13-62i: every mode, Takes too (its take's zones).
        req.pieces = PieceApplyFor(sd, v, rs.part_value, false, zone_peaks);
    }
    req.part = rs.part_value;
    req.gain_db = rs.gain_db;  // ResolveEvent already added the frame's dB
    // 13-29 / 13-41: placed as the audition plays it (no take FX, pan, playrate / pitch of the source item),
    // forced to mono as the Sound says.
    req.as_auditioned = true;
    req.chan_mode = ChanModeFor(PartMono(sd, rs.part_value));  // 13-62 (decision 7): the part's own, else the Sound's
    req.keep_rate = n.sound.keep_rate;  // 13-68 v3: an Attached source's copies keep its rate and pitch
    req.blend_ms = rs.offset_ms;
    return req;
}

void RemovePopulated(EventSounds& ev)
{
    std::vector<SoundPlacement> keep;
    for (const SoundPlacement& p : ev.placed) {
        if (p.own == Ownership::Populated) RemoveIndexed(p.item_guid);
        else keep.push_back(p);
    }
    ev.placed = keep;
}

// 13-35: what a Sync does (PopOpts::sync).
struct SyncRun {
    std::set<std::string>       scope;      // the behind nodes it syncs
    std::map<std::string, bool> overwrite;  // a hand-edited placed item (GUID, upper case) -> Overwrite (else Keep)
    SyncCounts*                 counts = nullptr;
};

// 13-35: the tag's Library prints and what RAV wrote, stamped from the store now (the item as it is).
void StampPlaced(MediaItem* it, const SoundStore& store, const SoundLibrary& lib, const std::string& node, int variation)
{
    PlacedTag tag;
    if (!it || !ReadPlacedTag(it, &tag)) return;
    tag.lib = PathPrints(store, lib, node, variation);
    tag.va = variation;
    tag.has_wr = true;
    tag.wr = ReadPlacedWritten(it);
    // A field kept unread stands for its value on write: dropped, so the new prints are written.
    tag.kept.erase(std::remove_if(tag.kept.begin(), tag.kept.end(),
                                  [](const std::string& k) {
                                      return k.compare(0, 4, "lib=") == 0 || k.compare(0, 3, "va=") == 0 ||
                                             k.compare(0, 3, "wr=") == 0;
                                  }),
                   tag.kept.end());
    WritePlacedTag(it, tag);
}

// 13-35: a placed item a Sync moved (a Blend offset, a sync point, a nudge put back): the follow's stored position follows,
// so the follow never reads the move as the user's nudge.
void SyncMovedFollow(MediaItem* it, double was_pos)
{
    const double pos = GetMediaItemInfo_Value(it, "D_POSITION");
    if (std::fabs(pos - was_pos) <= 1e-9) return;
    std::string buf(4096, '\0');
    if (!GetSetMediaItemInfo_String(it, kFollowStoreKey, &buf[0], false)) return;
    buf.resize(std::strlen(buf.c_str()));
    FollowStore fs;
    if (buf.empty() || !ParseFollowStore(buf, &fs)) return;
    fs.pos = pos;
    std::string text = FormatFollowStore(fs);
    GetSetMediaItemInfo_String(it, kFollowStoreKey, &text[0], true);
}

// 13-35: a placed item a Sync touches: its tag behind on a node of the run's scope. 13-35b: never a reshaped one (a
// structure change: Sync would place it from a stale shape; Populate places it again).
bool InSyncScope(MediaItem* it, const SoundStore& store, const SyncRun& run, PlacedTag* tag_out = nullptr)
{
    PlacedTag tag;
    if (!it || !ReadPlacedTag(it, &tag) || tag.own != Ownership::Populated) return false;
    if (tag_out) *tag_out = tag;
    if (ReshapeOf(tag, store).kind != Reshape::None) return false;
    for (const std::string& b : BehindNodes(tag, store))
        if (run.scope.count(b)) return true;
    return false;
}

// One item: prune, draw, place / replace / remove, write its record. `events` counts the events
// handled.
void PopulateOne(MediaItem* item, const SoundStore& store, const PopOpts& o, SoundReport& rep, int* events)
{
    const ItemDetection det = DetectItem(item);
    if (det.status != ItemDetection::Status::Ok) return;
    const std::string guid = ItemGuidOf(item);
    const std::vector<Block>& blocks = det.rules.blocks;
    ItemSounds rec = ReadItemSounds(item);
    {  // 13-68e: a rule holding a library with a RAV - Side Switch plays its side's value (given when it has none)
        std::vector<RuleSide> sides;
        for (const Block& b : blocks)
            if (!b.rid.empty()) sides.push_back({b.rid, EventSide(b)});
        if (FillMissingSideValues(store, rec, sides))
            ModifyItemSounds(item, nullptr, [&](ItemSounds& r) { return FillMissingSideValues(store, r, sides); });
    }
    // 13-15a: each rule's library and values come from the layers (rule, Character, Project).
    const ItemRigInfo     rig = ItemRig(item);
    const SoundCharacter* ch = CharacterFor(store, rig.skeleton, rig.model);
    std::map<std::string, std::pair<std::string, std::string>> part_of;  // rid -> body part, type
    std::map<std::string, std::string> role_of;  // 13-23 (D1): rid -> its events' role key
    std::map<std::string, char>        side_of;  // 13-73: rid -> its events' side (Left / Right)
    for (const Block& b : blocks)
        if (!b.rid.empty()) {
            std::pair<std::string, std::string> pt;
            PartOfBlock(b, &pt.first, &pt.second);
            part_of[b.rid] = pt;
            role_of[b.rid] = EventRoleKey(b);
            side_of[b.rid] = EventSide(b);
        }
    MeasureCache measured_here;  // 13-25: outside a gesture (PlacedDataFor)

    // P1: assignments of rules that no longer exist.
    std::set<std::string> live;
    for (const Block& b : blocks)
        if (!b.rid.empty()) live.insert(b.rid);
    const OrphanPrune pr = o.fill_passes || o.sync ? OrphanPrune{} : PruneOrphanRules(rec, live);  // 13-35: a Sync never prunes
    for (const std::string& g : pr.remove_guids) RemoveIndexed(g);
    for (const RuleSounds& r : pr.rules) {  // Antho 2026-10-12: removed and listed, by every gesture
        if (r.library.empty()) continue;  // 13-15a: inherit / None: no library was unassigned
        const SoundLibrary* lib = FindLibrary(store, r.library);
        AddLine(rep, ReportKind::Detached, RuleGoneText(lib ? lib->name : r.library), "", guid, 0.0);
    }

    if (rec.seed == 0) rec.seed = RandomSeed();
    PopulateDrawState st;
    st.seed = rec.seed;
    if (o.carry) st.history = *o.carry;  // 13-68: the earlier items' draws
    struct CarryBack {
        const PopulateDrawState& st;
        std::map<std::string, std::vector<int>>* to;
        ~CarryBack()
        {
            if (to) *to = st.history;
        }
    } carry_back{st, o.carry};
    // 13-59: New variation per loop pass: each pass other than the first draws its own (its history
    // kept per pass, across the item's events in time order).
    const bool                               pass_draws = LoopPassDrawOf(store) == LoopPassDraw::New;
    std::map<long long, PopulateDrawState>   pass_states;
    std::set<std::string> blocked_rules;
    std::set<std::string> gone_rules;
    std::set<std::string> stale_rules;  // rules that pointed to a deleted library (read as inherit)
    std::map<std::string, bool>            file_missing;  // File Sound uid -> its file is gone
    std::map<std::string, FileSoundTarget> file_targets;  // File Sound uid -> where it is placed from
    std::map<std::string, std::string>     file_why;      // File Sound uid -> why it cannot be placed
    // 13-13 / 13-16: a Sound's track path: its Switch path (layout not Off), then its Blend layer
    // track (every layout); empty = its usual track (outside any Blend, no Switch path). Its track is
    // found or made once per gesture, once its source resolved (SwitchTrackFor).
    const TrackLayout layout = TrackLayoutOf(store);
    const std::string sep = TrackSeparatorOf(store);  // 13-17 N4
    struct SoundTracks {
        std::vector<TrackStep> path;
        bool                   in_blend = false;
    };
    std::map<std::string, SoundTracks> track_paths;  // library uid + Sound uid -> its path
    auto switch_path_for = [&](const SoundLibrary& L, const SoundNode& n) -> const SoundTracks& {
        const std::string key = L.uid + "/" + n.uid;
        auto              hit = track_paths.find(key);
        if (hit == track_paths.end()) {
            SoundTracks st;
            if (L.attached) return track_paths.emplace(key, st).first->second;  // 13-68 v2: its sources' own tracks
            const BlendLayer layer = LayerOf(L, n.uid);
            st.in_blend = layer.in_blend;
            st.path = SoundTrackPath(layout == TrackLayout::Off ? std::vector<SwitchStep>() : SwitchPathOf(store, L, n.uid),
                                     layer, layout, EffectiveLandsIn(L, n.uid), L.uid, L.name, sep);
            hit = track_paths.emplace(key, st).first;
        }
        return hit->second;
    };

    // The events and their passes: the one list the follow (13-9) also reads (SoundEventsOf).
    const std::vector<FollowEvent> sound_events =
        SoundEventsOf(BuildEventList(det.events, det.rules.events, blocks.size()), blocks, det.map);
    const int n_events = static_cast<int>(sound_events.size());
    int       handled = 0;
    // Resilience (Antho 2026-10-19, "not resilient to timeline manipulations"): the record brought back in line with the
    // project before anything is drawn. The copies gone: deleted by hand, or forgotten when their track was deleted
    // or every copy went at once (SettleGoneCopies, as the follow). RAV's copies of this item the record does not
    // list are taken back (a re-run never stacks a second copy beside them); the record events that match no
    // event any more (Detect moved or dropped them) lose their copies.
    {
        const std::set<std::string> tracks = ProjectTrackGuids();
        SettleGoneCopies(
            rec, [](const std::string& g) { return FindItemByGuid(g) != nullptr; },
            [&tracks](const std::string& t) { return tracks.count(GuidKey(t)) > 0; });
        std::vector<LiveEvent> live_events;
        for (const FollowEvent& e : sound_events) live_events.push_back(LiveEvent{e.rid, e.t});
        // Review 2026-10-19: each stray's passes of its time and Blend offset (its node's library), so a copy the user
        // made of a sound (a duplicate, a paste, moved: off its passes) is left alone.
        std::vector<StrayCopy> strays = PopulatedCopiesOf(guid);
        for (StrayCopy& c : strays) {
            for (const FollowPass& fp : FollowPassesOf(det.map, c.t)) c.passes.push_back(fp.project_t);
            const auto ro = role_of.find(c.rid);
            for (const SoundLibrary& L : store.libraries)
                if (FindNode(L, c.node)) {
                    c.blend_ms = BlendOffsetMs(L, c.node, ro == role_of.end() ? std::string() : ro->second);
                    break;
                }
        }
        const StrayAdoption ad = AdoptStrayCopies(rec, strays, live_events, !o.fill_passes && !o.sync);
        for (const std::string& g : ad.remove_guids) RemoveIndexed(g);
        if (!o.fill_passes && !o.sync)  // a fill never removes (13-35: nor a Sync)
            for (const std::string& g : PruneStaleEvents(rec, live_events)) RemoveIndexed(g);
    }
    // 13-62i (D): what this Populate does with each event (PlanPopulateEvent: drawn, kept -- left out by the filter or
    // placed by the user, its draws still playing --, or skipped); each draw also avoids the picks the kept events
    // after it play (SetPopulateTimeline).
    PopulateFilter filter;
    filter.rid = o.rid;
    filter.rids = o.rids;
    filter.has_t = o.has_t;
    filter.t = o.t;
    filter.only_populated = o.only_populated;
    filter.follow_item = o.follow_assign && ItemIsPopulated(rec);  // Resilience 2026-10-19
    std::vector<EventPlan> plans;
    {
        std::vector<const EventSounds*> evs;
        for (const FollowEvent& e : sound_events) {
            const EventSounds* ev = FindEventSounds(rec, e.rid, e.t);
            evs.push_back(ev);
            EventPlan plan = PlanPopulateEvent(filter, e.rid, e.t, ev, e.enabled);
            if (o.sync && plan == EventPlan::Draw) {  // 13-35: only the events holding a sound behind in its scope
                bool behind = false;
                if (ev)
                    for (const SoundPlacement& p : ev->placed)
                        if (p.own == Ownership::Populated && InSyncScope(FindItemByGuid(p.item_guid), store, *o.sync)) behind = true;
                plan = SyncEventPlan(plan, ev != nullptr, behind);
            }
            plans.push_back(plan);
        }
        SetPopulateTimeline(st, pass_draws ? &pass_states : nullptr, plans, evs);
    }
    for (const FollowEvent& e : sound_events) {
        if (o.on_event) o.on_event(handled, n_events);
        else BusyStep(handled, n_events);  // 13-59: the busy card of the gesture running this (if any)
        const int at = handled++;  // 13-62i (D): its index in the timeline
        const std::string& rid = e.rid;
        EventSounds* ev = FindEventSounds(rec, rid, e.t);
        const EventPlan plan = static_cast<size_t>(at) < plans.size() ? plans[static_cast<size_t>(at)] : EventPlan::Skip;
        if (plan == EventPlan::Skip) continue;
        if (plan == EventPlan::Keep) {  // 13-62i (D): it still plays its recorded draws: they feed the history
            if (ev) SeedHistory(st, pass_draws ? &pass_states : nullptr, ev->draws);
            continue;
        }

        const auto            po = part_of.find(rid);
        const EffectiveAssign ea = po == part_of.end()
                                       ? EffectiveAssign()
                                       : ResolveAssignFor(store, rec, ch, rid, po->second.first, po->second.second, e.t);
        if (!ea.stale_library.empty()) stale_rules.insert(rid);
        const SoundLibrary*   lib = !ea.library.empty() ? FindLibrary(store, ea.library) : nullptr;
        if (!lib && (o.fill_passes || o.sync)) continue;  // 13-56 review: a fill never removes (13-35: nor a Sync)
        if (!lib) {  // P8: library None (or nothing set) removes the populated sounds
            const bool had = ev && EventIsPopulated(*ev);
            if (ev) RemovePopulated(*ev);
            if (!ea.library.empty() && gone_rules.insert(rid).second)  // a deleted library: said
                AddLine(rep, ReportKind::Blocked,
                        had ? "Library gone: its placed sounds removed" : "Library gone: nothing placed", "", guid,
                        e.t);
            continue;
        }
        const LibraryCheck chk = CheckLibrary(store, *lib);
        if (chk.blocked) {
            if (blocked_rules.insert(rid).second)
                AddLine(rep, ReportKind::Blocked, "Library \"" + lib->name + "\" blocked: " + chk.reason, "", guid, e.t);
            continue;
        }
        if (!ev) {
            EventSounds fresh;
            fresh.rid = rid;
            fresh.t = e.t;
            rec.events.push_back(fresh);
            ev = &rec.events.back();
        }
        if (events) ++*events;

        // P9: a recorded item no longer in the project was deleted by hand (Resilience 2026-10-19: settled above, SettleGoneCopies).
        if (o.revive_deleted) ForgetDeletedMarks(*ev);

        // P10: the draw.
        st.lib = lib;
        st.at = at;
        const std::vector<SoundDraw> prev = ev->draws;
        EventDraw ed;
        ed.rid = rid;
        ed.t = ev->t;
        ed.prev = &prev;
        if (const auto so = side_of.find(rid); so != side_of.end()) ed.side = so->second;  // 13-73
        const std::string lib_uid = lib->uid;
        auto is_missing = [&file_missing](const SoundNode& n) {
            // Spec missing-sounds-window: the one rule (an item Sound whose item is gone plays the file it remembers).
            if (n.sound.source.kind == SoundSourceKind::Item && !ItemGoneHere(n)) return false;
            auto it = file_missing.find(n.uid);  // 13-11: its file (no side effect here)
            if (it == file_missing.end()) it = file_missing.emplace(n.uid, SoundNodeMissing(n)).first;
            return it->second;
        };
        ValueLayers layers = ea.layers;
        if (const auto ro = role_of.find(rid); ro != role_of.end()) layers.role = ro->second;  // 13-23: picks the part
        const Resolution res = ResolveEvent(store, *lib, layers, MakeDrawHooks(st, ed, is_missing));
        std::vector<SoundDraw> draws = ed.out;
        for (const SoundDraw& d : prev) {  // a hand pick on a branch not walked now is kept
            bool have = false;
            for (const SoundDraw& x : draws)
                if (x.node == d.node) have = true;
            if (!have && d.hand) draws.push_back(d);
        }
        if (!o.fill_passes) {  // 13-56 review: a fill keeps the event's draw as it is
            // 13-59: the loop passes' own picks are kept (those drawn below replace theirs). Review: only
            // those of the passes the event still has (none in Same mode), so the record does not grow
            // with passes cut off; a pass stretched back is seeded again from what it plays (the fill).
            std::set<long long> keep_passes;
            if (pass_draws)
                for (const FollowPass& fp : e.passes) keep_passes.insert(fp.id);
            for (const SoundDraw& d : prev) {
                long long p = 0;
                if (ParsePassDrawKey(d.node, &p, nullptr) && keep_passes.count(p)) draws.push_back(d);
            }
            ev->draws = draws;
        }
        if (res.blocked) {
            AddLine(rep, ReportKind::Blocked, res.reason.empty() ? std::string("blocked") : res.reason, "", guid, e.t);
            continue;
        }
        for (const std::string& note : res.notes) {  // 13-62i (D, P1): e.g. a Blend without layer roles, once per Populate
            const bool said = std::any_of(rep.lines.begin(), rep.lines.end(),
                                          [&note](const ReportLine& l) { return l.kind == ReportKind::Info && l.text == note; });
            if (!said) AddLine(rep, ReportKind::Info, note, "", guid, e.t);
        }
        if (res.sounds.empty() && !res.gap.empty())  // 13-21: a library gap; the planner removes what it had placed
            AddLine(rep, ReportKind::NoSound, "Nothing placed: " + res.gap, res.gap_node, guid, e.t);

        std::vector<double> passes;
        for (const FollowPass& fp : e.passes) passes.push_back(fp.project_t);
        std::vector<DesiredSlot>   desired;
        std::vector<const ResolvedSound*> desired_rs;
        std::set<std::string>      hold;  // missing / no active variation: placed copies are kept
        // 13-59: each line once per event, whichever pass's draw met it.
        std::set<std::string> said;
        auto say = [&](ReportKind kind, const std::string& text, const std::string& node) {
            if (said.insert(std::to_string(static_cast<int>(kind)) + "\x1f" + node + "\x1f" + text).second)
                AddLine(rep, kind, text, node, guid, e.t);
        };
        // The desired slots of one draw (`r`, `edr`) on the passes `on` (indices into `passes`).
        auto add_desired = [&](const Resolution& r, const EventDraw& edr, const std::vector<int>& on) {
            for (const ResolvedSound& s : r.sounds) {
                const SoundNode* n = FindNode(*lib, s.node);
                if (!n) continue;
                if (s.missing) {
                    say(ReportKind::Missing,
                        n->sound.source.kind == SoundSourceKind::File ? "File not found: " + n->sound.source.asset.name
                                                                      : "Source item missing: " + NodeName(n),
                        n->uid);
                    hold.insert(n->uid);
                    continue;
                }
                if (edr.no_active.count(n->uid)) {
                    say(ReportKind::Missing, NodeName(n) + ": no active variation", n->uid);
                    hold.insert(n->uid);
                    continue;
                }
                if (const auto np = edr.no_part.find(n->uid); np != edr.no_part.end()) {
                    // 13-23 (A5): nothing of that part left in any variation: nothing placed (what was is removed).
                    say(ReportKind::NoSound, NoPartText(NodeName(n), DivideValueName(store, n->sound, np->second)), n->uid);
                    continue;
                }
                // 13-20: the items it is placed as (one per span), compared with what is placed; a folder
                // pile is not counted (its members are the pile's). 13-25: the snap its anchor
                // piece gets (item s), compared too, so a sync point measured or moved since is placed again.
                // 13-62i: Takes mode too (its take's zones).
                int    pieces = 0;
                double sync = -1.0;
                bool   pile = false;
                double rate = 1.0;
                std::vector<PieceApply> pa;
                if (n->sound.source.kind == SoundSourceKind::Item)
                    if (MediaItem* src = FindItemByGuid(n->sound.source.item_guid)) {
                        pile = IsFolderItem(src);
                        // 13-68 v3: the copy's rate: its source's when it keeps it (keep_rate), else 1 (PlayAsAuditioned);
                        // Takes mode: the variation's take's (the one it plays)
                        if (n->sound.keep_rate)
                            rate = TakePlayrateOf(src, n->sound.mode == DetectMode::Takes ? s.variation : -1);  // 13-68 v3
                    }
                if (s.variation >= 0 && pile) pa = PieceApplyFor(n->sound, s.variation, s.part_value);  // 13-62i: its zone's gain
                if (s.variation >= 0) {
                    if (!pile) {
                        const SoundData& sd = PlacedDataFor(
                            lib_uid, *n, s.variation, [n, &s]() { return PlacedSourcePath(*n, s.variation); }, measured_here);
                        const PeakData* zpk = ZoneSnapPeaks(
                            *n, s.variation, s.part_value, [n, &s]() { return PlacedSourcePath(*n, s.variation); }, measured_here);  // the file the syncs above read
                        pa = PieceApplyFor(sd, s.variation, s.part_value, PlaysFromFile(*n), zpk);
                        pieces = static_cast<int>(pa.size());
                        if (!(rate > 0.0) || !std::isfinite(rate)) rate = 1.0;
                        for (const PieceApply& a : pa)
                            if (a.anchor) sync = a.slice.snap_offset / rate;
                    }
                }
                // 13-62i (D, P2): each item's dB now (a pile: one, its anchor zone's gain on top).
                const std::vector<double> piece_db =
                    pile ? std::vector<double>{PileGainDb(pa, s.gain_db)} : PieceDbFor(pa, s.gain_db);
                for (int p : on) {
                    DesiredSlot d;
                    d.pass = p;
                    d.node = n->uid;
                    d.variation = s.variation;
                    d.part = s.part_value;
                    d.pieces = pieces;
                    d.sync = sync;
                    d.piece_db = piece_db;
                    d.pile = pile;
                    desired.push_back(d);
                    desired_rs.push_back(&s);
                }
            }
        };
        // 13-59: Same (or the first pass): the event's draw; New: every other pass its own.
        std::vector<int> main_passes;
        struct PassDraw {
            long long              id = 0;
            std::vector<int>       on;
            std::vector<SoundDraw> prev;
            EventDraw              ed;
            Resolution             res;
        };
        std::deque<PassDraw> pass_draws_here;  // stable addresses: desired_rs points into their res
        for (size_t p = 0; p < e.passes.size(); ++p) {
            const long long id = e.passes[p].id;
            if (!pass_draws || id == 0) {
                main_passes.push_back(static_cast<int>(p));
                continue;
            }
            PassDraw* pd = nullptr;
            for (PassDraw& x : pass_draws_here)
                if (x.id == id) pd = &x;
            if (!pd) {
                pass_draws_here.emplace_back();
                pd = &pass_draws_here.back();
                pd->id = id;
            }
            pd->on.push_back(static_cast<int>(p));
        }
        add_desired(res, ed, main_passes);
        for (PassDraw& pd : pass_draws_here) {
            PopulateDrawState& ps = pass_states[pd.id];
            ps.lib = lib;
            ps.seed = PassDrawSeed(rec.seed, pd.id);
            ps.at = at;
            pd.prev = PassPrevDraws(prev, pd.id);
            pd.ed.rid = rid;
            pd.ed.t = ev->t;
            pd.ed.prev = &pd.prev;
            pd.ed.side = ed.side;  // 13-73
            pd.res = ResolveEvent(store, *lib, layers, MakeDrawHooks(ps, pd.ed, is_missing));
            if (pd.res.blocked) continue;
            // Review: a fill records a pass's picks only once it knows what that pass plays (below).
            if (!o.fill_passes) MergePassDraws(ev->draws, pd.id, pd.ed.out);
            add_desired(pd.res, pd.ed, pd.on);
        }
        std::vector<ExistingSlot> existing;
        std::vector<std::pair<int, PlacedPick>> live_picks;  // 13-59 review: pass index -> what plays there
        for (const SoundPlacement& p : ev->placed) {
            ExistingSlot x;
            x.node = p.node;
            x.variation = p.variation;
            x.own = p.own;
            if (p.own == Ownership::Populated && hold.count(p.node)) x.own = Ownership::Attached;  // planner: leave it
            if (x.own == Ownership::Populated)
                if (MediaItem* it = FindItemByGuid(p.item_guid)) {
                    PlacedTag tag;
                    if (ReadPlacedTag(it, &tag)) {
                        x.part = tag.pt;  // 13-23: the part it plays
                        if (tag.has_anchor) x.sync = tag.sy;  // 13-25: the snap it was placed by
                    }
                    double anchor = 0.0, sync = 0.0;  // the pile's anchor: one pass for the whole pile
                    PlacedPileAnchor(it, &anchor, &sync);
                    x.pass = NearestPassOfItem(passes, anchor, sync, BlendOffsetMs(*lib, p.node, layers.role));  // 13-31
                    live_picks.push_back({x.pass, PlacedPick{p.node, p.variation, x.part}});
                }
            existing.push_back(x);
        }

        std::set<int>               erase;
        std::vector<SoundPlacement> added;
        std::vector<std::pair<int, PlacedPick>> filled_picks;  // 13-59 review: pass index -> placed by this fill
        if (o.counts) {  // a sound deleted by hand is never placed again (PlanEventSlots): said in the report
            std::set<std::string> deleted;
            for (const ExistingSlot& x : existing)
                if (x.own == Ownership::Deleted) deleted.insert(x.node);
            std::set<std::string> live;
            for (const ExistingSlot& x : existing)
                if (x.own == Ownership::Populated) live.insert(x.node);
            for (const DesiredSlot& d : desired)
                if (deleted.count(d.node) && !live.count(d.node)) ++o.counts->deleted_kept;
        }
        // 13-35: a Sync's own state for this event: its placements behind in scope (by index), those it touched, and how
        // each placed item's hand edit is met.
        std::set<int> sync_in, sync_done;
        if (o.sync)
            for (size_t i = 0; i < ev->placed.size(); ++i)
                if (ev->placed[i].own == Ownership::Populated && InSyncScope(FindItemByGuid(ev->placed[i].item_guid), store, *o.sync))
                    sync_in.insert(static_cast<int>(i));
        auto sync_touches = [&](const SlotStep& step) {
            for (int i : step.existing)
                if (sync_in.count(i)) return true;
            return false;
        };
        auto sync_overwrite = [&](const std::string& item_guid) {
            const auto hit = o.sync->overwrite.find(GuidKey(item_guid));
            return hit != o.sync->overwrite.end() && hit->second;
        };
        // 13-35: a Keep step of a Sync: the slot's items brought to the Library's settings in place (their span, snap,
        // fades and volume, then their position: the Blend offset and the sync point), each hand edit met as chosen;
        // their tags stamped. Pieces, a single item or a folder pile (its volume and position alone).
        auto sync_keep = [&](const SlotStep& step) {
            const DesiredSlot&   kd = desired[static_cast<size_t>(step.desired)];
            const ResolvedSound& s = *desired_rs[static_cast<size_t>(step.desired)];
            const SoundNode*     n = FindNode(*lib, kd.node);
            if (!n || kd.pass < 0 || static_cast<size_t>(kd.pass) >= passes.size()) return;
            const bool             file_kind = PlaysFromFile(*n);
            const int              v = s.variation;
            std::vector<PieceApply> pa;
            SliceApply              slice;
            FadeApply               fades;
            if (!kd.pile && v >= 0) {
                const SoundData& sd =
                    PlacedDataFor(lib_uid, *n, v, [n, v]() { return PlacedSourcePath(*n, v); }, measured_here);
                const PeakData* zpk =
                    ZoneSnapPeaks(*n, v, s.part_value, [n, v]() { return PlacedSourcePath(*n, v); }, measured_here);
                pa = PieceApplyFor(sd, v, s.part_value, file_kind, zpk);
                if (file_kind) {
                    slice = FileSliceFor(sd, v);
                    fades = FileFadeApplyFor(sd, v);
                } else if (v < static_cast<int>(sd.frames.size())) {
                    if (sd.mode == DetectMode::TakeMarkers || sd.mode == DetectMode::Silence)
                        slice = SliceApplyFor(sd.frames[static_cast<size_t>(v)]);
                    fades = FadeApplyFor(sd, v);
                }
            }
            struct One {
                MediaItem*        it;
                int               idx;
                PlacedTag         tag;
                const PieceApply* piece;
                SyncItemPlan      plan;
                bool              hand;
                bool              over;
                double            rate;
            };
            std::vector<One> ones;
            bool             hand_any = false, over_any = false;
            double           nudge = 0.0;
            for (int i : step.existing) {
                const SoundPlacement& pl = ev->placed[static_cast<size_t>(i)];
                MediaItem*            it = FindItemByGuid(pl.item_guid);
                PlacedTag             tag;
                if (!it || !ReadPlacedTag(it, &tag)) continue;
                One one{it, i, tag, nullptr, SyncItemPlan{}, false, false, 1.0};
                if (MediaItem_Take* tk = GetActiveTake(it)) one.rate = GetMediaItemTakeInfo_Value(tk, "D_PLAYRATE");
                if (!(one.rate > 0.0) || !std::isfinite(one.rate)) one.rate = 1.0;
                const int pi = tag.pi < 0 ? 0 : tag.pi;
                if (!kd.pile && pi < static_cast<int>(pa.size())) one.piece = &pa[static_cast<size_t>(pi)];
                const PlacedWrite now = ReadPlacedWritten(it);
                const HandEdit    hand = tag.has_wr ? HandEdits(tag.wr, now, pl.offset_s) : HandEdit{};
                one.hand = hand.Any();
                one.over = one.hand && sync_overwrite(pl.item_guid);
                const double want = kd.piece_db.empty() ? (tag.has_g ? tag.g : 0.0)
                                                        : kd.piece_db[std::min(static_cast<size_t>(pi), kd.piece_db.size() - 1)];
                const PlacedWrite wr = tag.has_wr ? tag.wr : now;
                one.plan = PlanSyncItem(wr, now, hand, one.over, want, tag.has_g ? tag.g : want, pl.offset_s);
                one.tag.has_g = true;
                one.tag.g = want;
                if (ones.empty()) nudge = one.plan.nudge_s;
                hand_any = hand_any || one.hand;
                over_any = over_any || one.over;
                ones.push_back(one);
            }
            if (ones.empty()) return;
            for (One& one : ones) {
                PlacedUpdate u;
                u.piece = one.piece;
                if (!one.piece && !kd.pile && pa.empty()) {
                    u.slice = &slice;
                    u.fades = &fades;
                }
                u.set_fades = one.plan.set_fades;
                u.set_length = one.plan.set_length;
                u.vol = one.plan.vol;
                UpdatePlacedItem(one.it, u);
            }
            // Where it lands now: the event's pass, the snap of its anchor (a pile: the snap it was placed by), the Blend
            // offset now, the nudge (Keep) or none (Overwrite).
            double sync = ones.front().tag.has_anchor ? ones.front().tag.sy : GetMediaItemInfo_Value(ones.front().it, "D_SNAPOFFSET");
            if (!kd.pile) {
                const One* anchor = nullptr;
                for (const One& one : ones)
                    if (one.piece ? one.piece->anchor : !anchor) anchor = &one;
                if (anchor) sync = GetMediaItemInfo_Value(anchor->it, "D_SNAPOFFSET");
            }
            const double at = PlacementPosition(passes[static_cast<size_t>(kd.pass)], sync, s.offset_ms, nudge);
            for (One& one : ones) {
                double pos = at + (one.tag.has_anchor ? one.tag.ao : 0.0);
                if (one.piece) pos = one.piece->anchor ? at : PiecePosition(at, one.piece->from_anchor, one.rate);
                const double was_pos = GetMediaItemInfo_Value(one.it, "D_POSITION");
                SetPlacedPosition(one.it, pos);
                SyncMovedFollow(one.it, was_pos);
                const PlacedWrite now = ReadPlacedWritten(one.it);
                PlacedTag&        t = one.tag;
                PlacedWrite       wr = t.has_wr ? t.wr : now;
                wr.vol = one.plan.wr_vol;
                if (one.plan.set_fades) {
                    wr.fade_in = now.fade_in;
                    wr.fade_out = now.fade_out;
                    wr.in_shape = now.in_shape;
                    wr.out_shape = now.out_shape;
                    wr.in_dir = now.in_dir;
                    wr.out_dir = now.out_dir;
                }
                if (one.plan.set_length) wr.length = now.length;
                t.has_anchor = true;
                t.ao = GetMediaItemInfo_Value(one.it, "D_POSITION") - at;
                t.sy = sync;
                t.lib = PathPrints(store, *lib, kd.node, v);
                t.va = v;
                t.has_wr = true;
                t.wr = wr;
                WritePlacedTag(one.it, t);
                ev->placed[static_cast<size_t>(one.idx)].offset_s = nudge;
                sync_done.insert(one.idx);
            }
            if (o.sync->counts) {
                ++o.sync->counts->adjusted;
                if (hand_any) ++(over_any ? o.sync->counts->overwritten : o.sync->counts->kept);
            }
        };
        for (const SlotStep& step : o.fill_passes ? PlanPassFill(desired, existing) : PlanEventSlots(desired, existing)) {
            if (o.sync) {  // 13-35: Sync keeps the draw: no new sound, none removed, no node replaced by another
                const SyncStep sa = SyncStepAction(step.act, sync_touches(step));
                if (sa == SyncStep::Skip) continue;
                if (sa == SyncStep::KeepInPlace) {
                    sync_keep(step);
                    continue;
                }
            }
            if (step.act == SlotAction::Keep) {
                if (o.counts) ++o.counts->kept;
                // 13-16 (B7c): a kept item still on its RAV track that now overlaps (a follow
                // move) goes to a free spill child of the same track.
                // 13-62h: its pieces crossfading at a Gap are not an overlap (CrossfadeAsButted).
                std::vector<MediaItem*> kept_items;
                for (int i : step.existing)
                    if (MediaItem* it = FindItemByGuid(ev->placed[static_cast<size_t>(i)].item_guid)) kept_items.push_back(it);
                const DesiredSlot& kd = desired[static_cast<size_t>(step.desired)];
                if (!o.fill_passes)  // 13-35: a placement older than the prints: stamped as it plays now (Sync then follows it)
                    for (MediaItem* it : kept_items) {
                        PlacedTag tag;
                        if (!ReadPlacedTag(it, &tag)) continue;
                        if (tag.lib.empty()) {
                            StampPlaced(it, store, *lib, kd.node, kd.variation);
                        } else if (AdoptStructure(tag.lib, PathPrints(store, *lib, kd.node, tag.va))) {
                            // 13-35b: Populate kept it where the Library's structure now puts it: its path and structure
                            // prints are the current ones (the reshaped mark clears), its settings stay as placed.
                            tag.kept.erase(std::remove_if(tag.kept.begin(), tag.kept.end(),
                                                          [](const std::string& k) { return k.compare(0, 4, "lib=") == 0; }),
                                           tag.kept.end());
                            WritePlacedTag(it, tag);
                        }
                    }
                CrossfadeAsButted as_butted(kept_items);
                for (MediaItem* it : kept_items) FixLaneOverlap(nullptr, it);
                // 13-62i (D, P2): a kept item plays at the library's gain now (a volume changed since it was placed).
                for (MediaItem* it : kept_items) {
                    PlacedTag tag;
                    const bool       tagged = ReadPlacedTag(it, &tag);
                    const double     vol_was = GetMediaItemInfo_Value(it, "D_VOL");
                    const KeptVolume kv = KeptItemVolume(kd.piece_db, kd.pile, static_cast<int>(step.existing.size()),
                                                         tagged && tag.has_g, tag.g, tag.pi, vol_was);
                    if (!kv.set || !f_set_item_value || !f_set_item_value(it, "D_VOL", kv.vol)) continue;
                    if (!tagged) continue;
                    // 13-35: the Library's volume is on it now: the prints that only changed their volume take it, and
                    // what RAV wrote follows (a hand edit of its volume stays one).
                    KeptTagAfterVolume(tag, PathPrints(store, *lib, kd.node, tag.va), vol_was, kv.vol);
                    tag.has_g = true;
                    tag.g = kv.g;
                    tag.pi = kv.pi;
                    WritePlacedTag(it, tag);
                }
                continue;
            }
            if (step.act == SlotAction::Remove) {
                if (o.counts && !step.existing.empty()) ++o.counts->removed;
                for (int i : step.existing) {
                    RemoveIndexed(ev->placed[static_cast<size_t>(i)].item_guid);
                    erase.insert(i);
                }
                if (o.report_replaced && !step.existing.empty()) {
                    const std::string& was = ev->placed[static_cast<size_t>(step.existing.front())].node;
                    AddLine(rep, ReportKind::Replaced, NodeName(FindNode(*lib, was)) + " -> (none)", was, guid, e.t);
                }
                continue;
            }
            const DesiredSlot&   d = desired[static_cast<size_t>(step.desired)];
            const ResolvedSound& s = *desired_rs[static_cast<size_t>(step.desired)];
            const SoundNode*     n = FindNode(*lib, d.node);
            if (!n || d.pass < 0 || static_cast<size_t>(d.pass) >= passes.size()) continue;
            const double  pt = passes[static_cast<size_t>(d.pass)];
            const SoundTracks&            sound_tracks = switch_path_for(*lib, *n);
            const std::vector<TrackStep>& track_path = sound_tracks.path;
            const std::string             track_mark = track_path.empty() ? std::string() : track_path.back().mark;
            MediaTrack*                   switch_track = nullptr;
            MediaTrack*                   pile_layer = nullptr;  // 13-16 B5: an NVK pile's layer track
            const FileSoundTarget*        file = nullptr;
            if (PlaysFromFile(*n)) {  // readied once per gesture, when first placed
                auto ft = file_targets.find(n->uid);
                if (ft == file_targets.end()) {
                    FileSoundTarget t;
                    std::string     why;
                    // 13-13: with a Switch track path, the file only (no unused "layer N" track); its
                    // Switch track once the file resolved, else (not made) its usual layer track.
                    bool ok = PrepareFileSound(nullptr, lib_uid, n->uid, &t, &why, track_path.empty());
                    if (ok && !track_path.empty() && !t.path.empty() &&
                        !SwitchTrackFor(track_path, rep, n->uid, guid, e.t)) {
                        t = FileSoundTarget{};
                        ok = PrepareFileSound(nullptr, lib_uid, n->uid, &t, &why, true);
                    }
                    if (!ok) file_why[n->uid] = why.empty() ? "File not found: " + n->sound.source.asset.name : why;
                    ft = file_targets.emplace(n->uid, t).first;
                }
                if (file_why.count(n->uid)) {
                    AddLine(rep, ReportKind::Missing, file_why[n->uid], n->uid, guid, e.t);
                    continue;
                }
                file = &ft->second;
                switch_track = SwitchTrackFor(track_path, rep, n->uid, guid, e.t);  // the gesture's (cached)
            } else if (!track_path.empty()) {
                // Only once the source item resolved. An NVK pile outside any Blend keeps its own
                // child tracks (13-13 T6); inside one, the layer track holds a child per pile track (B5).
                MediaItem* src = FindItemByGuid(n->sound.source.item_guid);
                if (src && !IsFolderItem(src)) switch_track = SwitchTrackFor(track_path, rep, n->uid, guid, e.t);
                else if (src && sound_tracks.in_blend) pile_layer = SwitchTrackFor(track_path, rep, n->uid, guid, e.t);
            }
            // 13-25 (D6): stale sync points measured now, over the file it plays.
            const SoundData& placed_data = PlacedDataFor(
                lib_uid, *n, s.variation,
                [file, n, &s]() { return file && !file->path.empty() ? file->path : PlacedSourcePath(*n, s.variation); },
                measured_here);
            // 13-62h review: measured on the file it places (as the syncs above), not the source's own path.
            const PeakData* zone_peaks = ZoneSnapPeaks(
                *n, s.variation, s.part_value,
                [file, n, &s]() { return file && !file->path.empty() ? file->path : PlacedSourcePath(*n, s.variation); },
                measured_here);
            PlaceRequest  req = RequestFor(item, rid, ev->t, pt, *n, s, file, &placed_data, zone_peaks);
            if (switch_track) {  // 13-13: every copy on its Switch-named track; 13-16: or its layer track
                req.track = switch_track;
                req.track_mark = track_mark;
            } else if (req.track && file && req.track == file->track) {
                req.track_mark = TrackMarkOf(req.track);  // 13-11's "layer N" track: RAV's, it spills too (B6)
            }
            if (lib->attached && n->sound.source.kind == SoundSourceKind::Item) {
                // 13-68 v2: an Attached library's sounds land on their source items' own tracks; a folder pile's
                // members on their own tracks. 13-68e (Antho 2026-10-19, "in on-the-fly mode never create a track"):
                // with no track mark, so they never settle on lanes (13-11 B6 is for the other libraries): no new
                // track or folder, no item moved; overlapping copies simply overlap on the user's track.
                if (MediaItem* src = FindItemByGuid(n->sound.source.item_guid)) {
                    req.track_mark.clear();
                    if (!IsFolderItem(src)) {
                        req.track = GetMediaItem_Track(src);
                    } else {
                        req.pile_track = [](MediaTrack* src_track, std::string* mark) -> MediaTrack* {
                            if (mark) mark->clear();
                            return src_track;
                        };
                    }
                }
            }
            if (pile_layer) {
                const std::string layer_mark = track_mark;
                req.pile_track = [pile_layer, layer_mark](MediaTrack* src_track, std::string* mark) -> MediaTrack* {
                    const std::string m = PileChildMark(layer_mark, TrackMarkOf(src_track));
                    MediaTrack*       t = EnsureMarkedChild(nullptr, pile_layer, TrackName(src_track), m);
                    if (t && mark) *mark = m;
                    return t;
                };
            }
            PlaceResult   r;
            if (step.act == SlotAction::Place) {
                r = PlaceSounds(nullptr, req);
            } else {
                // Replace in place, the user's nudge kept.
                const SoundPlacement& old = ev->placed[static_cast<size_t>(step.existing.front())];
                if (MediaItem* oi = FindItemByGuid(old.item_guid)) {
                    // A folder pile is placed from the folder's snap, its members offset from it:
                    // read back from the surviving member's tag (13-8).
                    const SoundLibrary& L = *lib;
                    double              anchor = 0.0, sync = 0.0;
                    PlacedPileAnchor(oi, &anchor, &sync);
                    req.offset_s = NudgeOf(anchor, pt, sync, BlendOffsetMs(L, old.node, layers.role));  // 13-31
                }
                // 13-35: a Sync placing a sound again (its variation switched off, its zones re-split, its sync point
                // moved): its nudge from the record (Keep; Overwrite: none), and its hand edits read before.
                HandEdit    sync_hand;
                PlacedWrite sync_first, sync_last;  // the old first / last item (by position): their fade-in / fade-out
                double      sync_ratio = 1.0;      // the user's own volume change (a ratio), when one could be read
                bool        sync_has_ratio = false, sync_over = false;
                if (o.sync) {
                    sync_over = sync_overwrite(old.item_guid);
                    req.offset_s = sync_over ? 0.0 : old.offset_s;
                    double first_pos = 0.0, last_pos = 0.0;
                    bool   any = false;
                    for (int i : step.existing) {
                        const SoundPlacement& pl = ev->placed[static_cast<size_t>(i)];
                        MediaItem*            oi = FindItemByGuid(pl.item_guid);
                        PlacedTag             ot;
                        if (!oi || !ReadPlacedTag(oi, &ot) || !ot.has_wr) continue;
                        const PlacedWrite now = ReadPlacedWritten(oi);
                        const HandEdit    h = HandEdits(ot.wr, now, pl.offset_s);
                        MergeHandEdit(sync_hand, h);
                        if (h.vol && !sync_has_ratio && ot.wr.vol != 0.0 && std::isfinite(now.vol / ot.wr.vol)) {
                            sync_ratio = now.vol / ot.wr.vol;
                            sync_has_ratio = true;
                        }
                        const double pos = GetMediaItemInfo_Value(oi, "D_POSITION");
                        if (!any || pos < first_pos) {
                            first_pos = pos;
                            sync_first = now;
                        }
                        if (!any || pos >= last_pos) {
                            last_pos = pos;
                            sync_last = now;
                        }
                        any = true;
                    }
                }
                const int sync_old_var = old.variation;
                r = ReplacePlacement(nullptr, old.item_guid, req);
                if (o.sync && r.ok) {
                    for (size_t k = 0; k < r.items.size(); ++k) StampPlaced(r.items[k], store, *lib, n->uid, s.variation);
                    if (sync_hand.Any() && !sync_over && !r.items.empty()) {  // Keep: the user's volume (relative), fades, length stay
                        if (sync_hand.vol && sync_has_ratio)
                            for (MediaItem* it : r.items) SetPlacedVolume(it, GetMediaItemInfo_Value(it, "D_VOL") * sync_ratio);
                        if (sync_hand.fades) {
                            MediaItem* first = r.items.front();
                            MediaItem* last = r.items.front();
                            for (MediaItem* it : r.items) {
                                if (GetMediaItemInfo_Value(it, "D_POSITION") < GetMediaItemInfo_Value(first, "D_POSITION")) first = it;
                                if (GetMediaItemInfo_Value(it, "D_POSITION") >= GetMediaItemInfo_Value(last, "D_POSITION")) last = it;
                            }
                            auto put = [](MediaItem* it, const PlacedWrite& w, bool in, bool out) {
                                FadeApply fa;
                                fa.set_in = fa.shape_in = in;
                                fa.set_out = fa.shape_out = out;
                                fa.in = w.fade_in;
                                fa.out = w.fade_out;
                                fa.in_shape = w.in_shape;
                                fa.out_shape = w.out_shape;
                                fa.in_dir = w.in_dir;
                                fa.out_dir = w.out_dir;
                                PlacedUpdate u;
                                u.fades = &fa;
                                u.vol = GetMediaItemInfo_Value(it, "D_VOL");
                                UpdatePlacedItem(it, u);
                            };
                            if (first == last) {
                                PlacedWrite w = sync_first;
                                w.fade_out = sync_last.fade_out;
                                w.out_shape = sync_last.out_shape;
                                w.out_dir = sync_last.out_dir;
                                put(first, w, true, true);
                            } else {
                                put(first, sync_first, true, false);
                                put(last, sync_last, false, true);
                            }
                        }
                        // Its length only for one item played again as one item of the same variation.
                        if (sync_hand.length && sync_old_var == s.variation && step.existing.size() == 1 && r.items.size() == 1 &&
                            f_set_item_value)
                            f_set_item_value(r.items[0], "D_LENGTH", sync_first.length);
                    }
                    if (o.sync->counts) {
                        ++(sync_old_var != s.variation ? o.sync->counts->redrawn : o.sync->counts->adjusted);
                        if (sync_hand.Any()) ++(sync_over ? o.sync->counts->overwritten : o.sync->counts->kept);
                    }
                    for (int i : step.existing) sync_done.insert(i);
                }
                for (size_t k = 0; k < step.existing.size(); ++k) {
                    const int i = step.existing[k];
                    if (k > 0 && r.ok) RemoveIndexed(ev->placed[static_cast<size_t>(i)].item_guid);
                    if (r.ok) erase.insert(i);
                }
                if (r.ok && step.act == SlotAction::ReplaceNode && o.report_replaced) {
                    const SoundNode* was = FindNode(*lib, old.node);
                    AddLine(rep, ReportKind::Replaced, NodeName(was) + " -> " + NodeName(n), n->uid, guid, e.t);
                }
            }
            if (!r.ok) {
                AddLine(rep, ReportKind::Missing, NodeName(n) + ": " + (r.error.empty() ? "not placed" : r.error), n->uid,
                        guid, e.t);
                continue;
            }
            if (!r.error.empty())  // 13-62h review: placed in part (a piece could not be made): recorded, and said
                AddLine(rep, ReportKind::Missing, NodeName(n) + ": " + r.error, n->uid, guid, e.t);
            if (o.counts) ++(step.act == SlotAction::Place ? o.counts->placed : o.counts->replaced);  // per sound, not per pile item
            if (g_items)
                for (size_t k = 0; k < r.item_guids.size() && k < r.items.size(); ++k)
                    (*g_items)[GuidKey(r.item_guids[k])] = r.items[k];
            CheckOwners(r.item_guids, guid, rep, n->uid, e.t);
            if (!o.sync)  // 13-35: what the Library looked like (a Sync stamped them before its kept edits)
                for (size_t k = 0; k < r.items.size(); ++k) StampPlaced(r.items[k], store, *lib, n->uid, s.variation);
            for (size_t k = 0; k < r.item_guids.size(); ++k) {
                SoundPlacement p;
                p.item_guid = r.item_guids[k];
                p.node = n->uid;
                p.own = Ownership::Populated;
                p.offset_s = req.offset_s;
                p.variation = s.variation;
                // Resilience 2026-10-19: its track, known at once (a copy whose track is deleted is forgotten, not deleted by hand).
                if (k < r.items.size() && r.items[k]) {
                    char buf[64] = {};
                    MediaTrack* tr = GetMediaItem_Track(r.items[k]);
                    if (tr && GetSetMediaTrackInfo_String(tr, "GUID", buf, false)) p.track_guid = buf;
                }
                added.push_back(p);
            }
            if (step.act == SlotAction::Place) filled_picks.push_back({d.pass, PlacedPick{n->uid, s.variation, d.part}});
        }
        // 13-59 review: a fill records only the passes it placed. A pass that played nothing gets its
        // new draw; a pass that already played keeps its entries, or (none yet) gets them from what it
        // plays now, so a later Populate keeps it as it is.
        if (o.fill_passes)
            for (const PassDraw& pd : pass_draws_here) {
                if (pd.res.blocked) continue;
                auto on_pass = [&pd](int p) { return std::find(pd.on.begin(), pd.on.end(), p) != pd.on.end(); };
                std::vector<PlacedPick> playing;
                bool                    was_live = false;
                for (const auto& lp : live_picks)
                    if (on_pass(lp.first)) {
                        was_live = true;
                        playing.push_back(lp.second);
                    }
                bool filled = false;
                for (const auto& fp : filled_picks)
                    if (on_pass(fp.first)) {
                        filled = true;
                        playing.push_back(fp.second);
                    }
                if (!was_live) {
                    if (filled) MergePassDraws(ev->draws, pd.id, pd.ed.out);
                } else if (!HasPassDraws(ev->draws, pd.id)) {
                    MergePassDraws(ev->draws, pd.id, PassDrawsFromPlaced(*lib, playing));
                }
            }
        if (o.sync && o.sync->counts) {  // 13-35: behind in scope but left as it is (no variation left on, a structure change)
            std::set<std::string> left;
            for (int i : sync_in)
                if (!sync_done.count(i)) left.insert(ev->placed[static_cast<size_t>(i)].node + "#" + std::to_string(existing[static_cast<size_t>(i)].pass));
            o.sync->counts->skipped += static_cast<int>(left.size());
        }
        std::vector<SoundPlacement> placed;
        for (size_t i = 0; i < ev->placed.size(); ++i)
            if (!erase.count(static_cast<int>(i))) placed.push_back(ev->placed[i]);
        placed.insert(placed.end(), added.begin(), added.end());
        ev->placed = placed;
    }

    if (!stale_rules.empty()) AddLine(rep, ReportKind::Info, StaleRulesText(static_cast<int>(stale_rules.size())), "", guid, 0.0);
    bool too_large = false;
    ModifyItemSounds(
        item, nullptr,
        [&](ItemSounds& s) {
            s = rec;
            return true;
        },
        &too_large);
    if (too_large)  // 13-59 review: said to the user, not only logged (its new sounds are not recorded)
        AddLine(rep, ReportKind::Blocked, kRecordTooLargeText, "", guid, 0.0);
}

// 13-59: PopulateOne over the members of an item's pool, under the busy card ("Updating sounds").
void PopulateMembers(const std::vector<MediaItem*>& members, const SoundStore& store, const PopOpts& o, SoundReport& rep)
{
    BusyScope busy("Updating sounds");
    std::map<std::string, std::vector<int>> carried;  // 13-68: avoid-repeat across the members, in time order
    PopOpts                                 oc = o;
    if (!oc.carry) oc.carry = &carried;
    const std::vector<MediaItem*> in_time = ByPosition(members);
    for (size_t k = 0; k < in_time.size(); ++k) {
        busy.Item(static_cast<int>(k), static_cast<int>(in_time.size()));
        PopulateOne(in_time[k], store, oc, rep, nullptr);
    }
}

// Gives every block a rid (P7), pooled, inside the caller's undo scope.
void EnsureRids(MediaItem* item)
{
    ItemRulesRead rr;
    if (!ReadItemRules(item, &rr) || !rr.valid) return;
    bool missing = false;
    for (const Block& b : rr.rules.blocks)
        if (b.rid.empty()) missing = true;
    if (!missing) return;
    std::string err;
    ModifyItemRules(item, "RAV: Set rule ids", [](ItemRules& r) { return EnsureRuleIds(&r); }, &err);
}

std::string RidOf(MediaItem* item, int block)
{
    ItemRulesRead rr;
    if (!ReadItemRules(item, &rr) || !rr.valid) return {};
    if (block < 0 || static_cast<size_t>(block) >= rr.rules.blocks.size()) return {};
    return rr.rules.blocks[static_cast<size_t>(block)].rid;
}

// Antho 2026-10-12: the auto marker key of the item's block ("" for a rule).
std::string AutoKeyOf(MediaItem* item, int block)
{
    ItemRulesRead rr;
    if (!ReadItemRules(item, &rr) || !rr.valid) return {};
    if (block < 0 || static_cast<size_t>(block) >= rr.rules.blocks.size()) return {};
    return AutoSoundKeyOf(rr.rules.blocks[static_cast<size_t>(block)]);
}

std::vector<Block> BlocksOf(MediaItem* item)
{
    ItemRulesRead rr;
    if (!ReadItemRules(item, &rr) || !rr.valid) return {};
    return rr.rules.blocks;
}

// A row gesture's rule side: rids for the item's blocks of these keys, `edit` on each pool
// member's record, then their populated events re-placed (P8). Inside the caller's undo scope.
void EditAutoRules(MediaItem* item, const std::vector<std::string>& keys, const SoundStore& store,
                   const std::function<bool(ItemSounds&, const std::vector<std::string>&)>& edit, SoundReport& rep)
{
    if (!item || !RavTakeOf(item)) return;
    if (AutoKeysNeedRids(BlocksOf(item), keys)) EnsureRids(item);
    const std::vector<std::string> rids = AutoKeyRids(BlocksOf(item), keys);
    if (rids.empty()) return;
    const std::vector<MediaItem*> members = WithPoolMembers({item});
    for (MediaItem* m : members) ModifyItemSounds(m, nullptr, [&](ItemSounds& s) { return edit(s, rids); });
    PopOpts o;  // 13-8 fix: every rule of the row in one pass (one detection per member, not one per rule)
    o.only_populated = true;
    o.follow_assign = true;  // Resilience 2026-10-19: every event of the rules follows the assignment
    o.rids = rids;
    PopulateMembers(members, store, o, rep);
}

// A gesture's library argument: "" = inherit, kLibNoneWord = None, else a library uid.
void LibraryArg(const std::string& arg, LibState* state, std::string* uid)
{
    if (arg.empty()) {
        *state = LibState::Inherit;
        uid->clear();
    } else if (arg == kLibNoneWord) {
        *state = LibState::None;
        uid->clear();
    } else {
        *state = LibState::Library;
        *uid = arg;
    }
}

std::string FileNameOfPath(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The RAV items of the project (the list first: re-placing events adds and removes items).
std::vector<MediaItem*> AllRavItems()
{
    std::vector<MediaItem*> out;
    const int               n = CountMediaItems(nullptr);
    for (int i = 0; i < n; ++i) {
        MediaItem* it = GetMediaItem(nullptr, i);
        if (it && RavTakeOf(it)) out.push_back(it);
    }
    return out;
}

// 13-66 review: rules to re-place besides the ones a store change reaches (Commit's item edits, pool members
// included): per item the rids, and the items whose every rule re-places (an Item values edit).
struct ExtraRids {
    std::map<MediaItem*, std::set<std::string>> rids;
    std::set<MediaItem*>                        whole;
};

// After a store edit (13-15a): the populated events of every RAV item whose effective library or
// values changed (ChangedRids) are re-placed. Inside the caller's undo scope and item index.
// `extra` (Commit) adds rules to re-place; the items then go in time order and the avoid-repeat draws are carried
// from item to item of one pool (as a pool's re-place does), never across unrelated items. `store_changed`
// false: the store was not edited, only `extra` is re-placed.
void ReplaceChangedRules(const SoundStore& before, SoundReport& rep, const ExtraRids* extra = nullptr, bool store_changed = true)
{
    const SoundStore after = GetSoundStore(nullptr);
    std::vector<MediaItem*> all = AllRavItems();
    std::map<MediaItem*, int> pool_of;
    std::map<int, std::map<std::string, std::vector<int>>> carried;  // per pool, in time order
    if (extra) {
        all = ByPosition(all);
        std::vector<int>              group;
        const std::vector<MediaItem*> grouped = WithPoolMembers(all, nullptr, &group);
        for (size_t k = 0; k < grouped.size() && k < group.size(); ++k) pool_of[grouped[k]] = group[k];
    }
    BusyScope busy("Updating sounds");  // 13-59: a library or value change re-placing many items
    for (size_t k = 0; k < all.size(); ++k) {
        MediaItem* it = all[k];
        busy.Item(static_cast<int>(k), static_cast<int>(all.size()));
        ItemRulesRead rr;
        if (!ReadItemRules(it, &rr) || !rr.valid) continue;
        std::set<std::string> rids;
        if (store_changed) {
            const ItemRigInfo rig = ItemRig(it);
            for (const std::string& r : ChangedRids(before, after, ReadItemSounds(it), rr.rules.blocks, rig.skeleton, rig.model))
                rids.insert(r);
        }
        bool whole = false;
        if (extra) {
            whole = extra->whole.count(it) > 0;
            const auto mine = extra->rids.find(it);
            if (mine != extra->rids.end()) rids.insert(mine->second.begin(), mine->second.end());
        }
        if (!whole && rids.empty()) continue;
        PopOpts o;
        o.only_populated = true;
        o.follow_assign = true;  // Resilience 2026-10-19: every event of the rules follows the assignment
        if (!whole) o.rids.assign(rids.begin(), rids.end());
        if (extra) {
            const auto p = pool_of.find(it);
            if (p != pool_of.end()) o.carry = &carried[p->second];
        }
        PopulateOne(it, after, o, rep, nullptr);
    }
}

// One store gesture: nothing happens (no undo point) when `edit` changes nothing; else the store is
// edited and the rules it changes are re-placed, in one undo point.
SoundReport StoreGesture(const char* undo_desc, const std::function<bool(SoundStore&)>& edit)
{
    SoundReport rep;
    try {
        const SoundStore before = GetSoundStore(nullptr);
        SoundStore       probe = before;
        if (!edit(probe)) return rep;
        {
            SoundUndoScope undo(nullptr, undo_desc);
            ItemIndexScope index;
            ModifySoundStore(nullptr, nullptr, edit);
            ReplaceChangedRules(before, rep);
        }
        UpdateArrange();
    } catch (...) {
        AddLine(rep, ReportKind::Blocked, "Internal error: the gesture stopped part way (Ctrl+Z undoes it)", "", "", 0.0);
    }
    return rep;
}

// The character of `ref` (stored at its first edit), `f` applied to it. A record made for an edit
// that changed nothing is dropped again.
bool EditCharacter(SoundStore& st, const CharacterRef& ref, const std::function<bool(SoundCharacter&)>& f)
{
    const size_t    had = st.characters.size();
    SoundCharacter* c = EnsureCharacter(st, ref);
    if (!c) return false;
    const bool changed = f(*c);
    if (!changed && st.characters.size() > had) st.characters.erase(st.characters.begin() + static_cast<long>(had), st.characters.end());
    return changed;
}

}  // namespace

void InitSoundPopulate(void* (*get_func)(const char* name))
{
    if (!get_func) return;
    f_set_selected = reinterpret_cast<SetMediaItemSelectedFn>(get_func("SetMediaItemSelected"));
    f_set_item_value = reinterpret_cast<SetItemValueFn>(get_func("SetMediaItemInfo_Value"));
}

ItemRigInfo ItemRig(MediaItem* item)
{
    ItemRigInfo r;
    try {
        if (!item) return r;
        MediaItem_Take* take = RavTakeOf(item);
        if (!take) return r;
        const std::string path = AnimPathOf(take);
        if (path.empty()) return r;
        r.file = FileNameOfPath(path);
        struct Rig {
            AssetFileStamp stamp;
            std::string    skeleton, model;
            int            bones = 0;
        };
        static std::map<std::string, Rig> cache;  // anim path -> its rig, while the file is unchanged
        const AssetFileStamp stamp = ReadAssetFileStamp(path);
        auto                 hit = cache.find(path);
        if (hit == cache.end() || hit->second.stamp != stamp) {
            // A changed file: its cached parse (if any) is the old one, so it is read again.
            if (hit != cache.end()) EvictCpuAsset(path);
            const std::shared_ptr<const CpuAsset> asset = AcquireCpuAsset(path);
            // Review 13-15b: a file with no skeleton (or that does not load) is kept too, as no rig,
            // until the file changes: not parsed again on every call.
            const SkeletonBones sk = asset ? SkeletonBonesOf(asset->skeleton) : SkeletonBones();
            std::vector<MeshShape> shapes;
            if (asset && !sk.names.empty())
                for (const CpuMesh& m : asset->meshes) {
                    MeshShape ms;
                    ms.verts = m.verts.size();
                    ms.indices = m.indices.size();
                    ms.material = m.materialIdx;
                    ms.skinned = m.skinned;
                    shapes.push_back(ms);
                }
            Rig rig;
            rig.stamp = stamp;
            rig.skeleton = sk.names.empty() ? std::string() : SkeletonKey(sk.names);
            rig.model = ModelSignature(shapes);
            rig.bones = static_cast<int>(sk.names.size());
            hit = cache.insert_or_assign(path, rig).first;
        }
        r.skeleton = hit->second.skeleton;
        r.model = hit->second.model;
        r.bones = hit->second.bones;
    } catch (...) {
        r = ItemRigInfo{};
    }
    return r;
}

std::vector<ProjectRig> ProjectRigs()
{
    std::vector<ProjectRig> out;
    try {
        for (MediaItem* it : AllRavItems()) {
            const ItemRigInfo rig = ItemRig(it);
            if (rig.skeleton.empty()) continue;
            ProjectRig* pr = nullptr;
            for (ProjectRig& x : out)
                if (x.skeleton == rig.skeleton) pr = &x;
            if (!pr) {
                out.emplace_back();
                pr = &out.back();
                pr->skeleton = rig.skeleton;
                pr->bones = rig.bones;
            }
            ++pr->items;
            RigModelCount* mc = nullptr;
            for (RigModelCount& m : pr->models)
                if (m.model == rig.model) mc = &m;
            if (!mc) {
                pr->models.emplace_back();
                mc = &pr->models.back();
                mc->model = rig.model;
            }
            ++mc->items;
            if (std::find(mc->files.begin(), mc->files.end(), rig.file) == mc->files.end()) mc->files.push_back(rig.file);
        }
    } catch (...) {
    }
    return out;
}

MediaItem* FindItemByGuid(const std::string& guid)
{
    if (guid.empty()) return nullptr;
    if (g_items) {  // inside a gesture: its map; a miss (an item added outside it) is looked up and kept
        auto hit = g_items->find(GuidKey(guid));
        if (hit != g_items->end()) return hit->second;
        // 13-19: a GUID not in the project (a sound deleted by hand, a missing source) is kept as none, so
        // each later lookup in the gesture does not scan the project again. Items the gesture places
        // overwrite their entry (PopulateOne); GUIDs are never reused.
        MediaItem* it = FindPlacedItem(guid);
        (*g_items)[GuidKey(guid)] = it;
        return it;
    }
    return FindPlacedItem(guid);
}

void SelectOnlyItem(MediaItem* item)
{
    if (!item || !f_set_selected) return;
    Main_OnCommand(kUnselectAllItems, 0);
    f_set_selected(item, true);
    UpdateArrange();
}

std::vector<MediaItem*> SelectedRavItems()
{
    std::vector<MediaItem*> out;
    const int n = CountSelectedMediaItems(nullptr);
    for (int i = 0; i < n; ++i) {
        MediaItem* it = GetSelectedMediaItem(nullptr, i);
        if (it && RavTakeOf(it)) out.push_back(it);
    }
    return out;
}

std::vector<MediaItem*> PopulateTargets(const std::vector<MediaItem*>& items)
{
    std::vector<MediaItem*> rav;
    for (MediaItem* it : items)
        if (it && RavTakeOf(it)) rav.push_back(it);
    if (rav.empty()) return rav;
    return WithPoolMembers(rav);
}

SoundReport PopulateItemsCounted(const std::vector<MediaItem*>& items, int* events, PopulateCounts* counts,
                                 bool revive_deleted, const PopulateProgressFn& progress,
                                 const std::vector<std::string>& only_rids)
{
    SoundReport rep;
    if (events) *events = 0;
    if (counts) *counts = PopulateCounts();
    try {
        const std::vector<MediaItem*> targets = ByPosition(PopulateTargets(items));
        if (targets.empty()) return rep;
        BusyScope busy("Populating sounds");  // 13-59: the busy card (Repopulate's own, for instance)
        PopulateProgress where;  // 13-19: the caller's progress frames, the first one before any work
        where.items = static_cast<int>(targets.size());
        if (progress) progress(where);
        const SoundStore store = GetSoundStore(nullptr);
        {
            SoundUndoScope undo(nullptr, revive_deleted ? "RAV: Restore deleted sounds" : "RAV: Populate sounds");
            ItemIndexScope index;
            PopOpts o;
            o.counts = counts;
            o.revive_deleted = revive_deleted;
            o.rids = only_rids;  // 13-68: only these rules (empty = every rule)
            std::map<std::string, std::vector<int>> carried;  // 13-68: avoid-repeat across the items
            o.carry = &carried;
            // 13-15: a rule that only inherits its sound (Project / Character) has no id until now: given first,
            // else its events are not placed (they are found by rule id).
            for (MediaItem* it : targets) EnsureRids(it);
            if (progress)
                o.on_event = [&where, &progress](int done, int total) {
                    where.event = done;
                    where.events = total;
                    progress(where);
                };
            for (size_t k = 0; k < targets.size(); ++k) {
                where.item = static_cast<int>(k);
                where.event = where.events = 0;
                if (progress) progress(where);
                else busy.Item(where.item, where.items);  // 13-59: PopulateOne steps its events itself
                PopulateOne(targets[k], store, o, rep, events);
            }
            RefreshRavTrackNames(nullptr);  // 13-17 N3: the RAV tracks take their Library names
        }
        if (counts) {  // the details list says it too
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%d sound%s placed, %d changed, %d removed, %d already in place", counts->placed,
                          counts->placed == 1 ? "" : "s", counts->replaced, counts->removed, counts->kept);
            AddLine(rep, ReportKind::Info, buf, "", "", 0.0);
            if (counts->deleted_kept > 0) {
                std::snprintf(buf, sizeof(buf),
                              "%d sound%s you deleted by hand kept (Restore, on the Populate line, places them again)",
                              counts->deleted_kept, counts->deleted_kept == 1 ? "" : "s");
                AddLine(rep, ReportKind::Info, buf, "", "", 0.0);
            }
        }
        UpdateArrange();
    } catch (...) {
        AddLine(rep, ReportKind::Blocked, "Internal error: the gesture stopped part way (Ctrl+Z undoes it)", "", "", 0.0);
    }
    return rep;
}

SoundReport PopulateItems(const std::vector<MediaItem*>& items)
{
    return PopulateItemsCounted(items, nullptr);
}

SoundReport PopulateLoopPassesNoUndo(const std::vector<MediaItem*>& items)
{
    SoundReport rep;
    try {
        const SoundStore store = GetSoundStore(nullptr);
        ItemIndexScope   index;
        PopOpts          o;
        o.only_populated = true;  // the events that already have their sounds, never a new one
        o.report_replaced = false;
        o.fill_passes = true;     // review: the missing passes placed, nothing else changed
        SoundReport quiet;        // review: no report lines (the follow's automatic fill says nothing)
        // 13-59 review: except a record too large to save, which the user must know of.
        BusyScope   busy("Filling loop passes");  // 13-59: a long stretch fills many passes
        for (size_t k = 0; k < items.size(); ++k) {
            MediaItem* it = items[k];
            busy.Item(static_cast<int>(k), static_cast<int>(items.size()));
            if (it && RavTakeOf(it)) PopulateOne(it, store, o, quiet, nullptr);
            for (const ReportLine& l : quiet.lines)
                if (l.text == kRecordTooLargeText) rep.lines.push_back(l);
            quiet.lines.clear();
        }
        RefreshRavTrackNames(nullptr);  // 13-17 N3, as Populate
        UpdateArrange();
    } catch (...) {
        AddLine(rep, ReportKind::Blocked, "Internal error: the loop passes were not all filled", "", "", 0.0);
    }
    return rep;
}

SoundReport SyncPlacedSounds(const SyncPlacedRequest& request, SyncCounts* counts)
{
    SoundReport rep;
    SyncCounts  local;
    SyncCounts& c = counts ? *counts : local;
    c = SyncCounts{};
    try {
        {
            SoundUndoScope undo(nullptr, "RAV: Sync placed sounds");
            ItemIndexScope index;
            BusyScope      busy("Syncing placed sounds");
            if (request.remember) {  // Don't ask again: the rule, in this undo point
                const HandEditRule rule = request.rule;
                ModifySoundStore(nullptr, nullptr, [rule](SoundStore& st) { return SetHandEditRule(st, rule); });
            }
            const SoundStore store = GetSoundStore(nullptr);
            std::map<std::string, std::vector<int>> carried;  // a redraw avoids repeating across the items
            // 13-35b: the items holding a reshaped sound in scope: the usual Populate (a new draw), in this undo point.
            std::set<std::string> again;
            {
                std::vector<MediaItem*> items;
                for (const std::string& g : request.populate)
                    if (MediaItem* it = FindItemByGuid(g))
                        if (RavTakeOf(it) && again.insert(GuidKey(g)).second) items.push_back(it);
                items = ByPosition(items);
                PopOpts p;
                p.report_replaced = false;
                p.carry = &carried;
                for (size_t k = 0; k < items.size(); ++k) {
                    busy.Item(static_cast<int>(k), static_cast<int>(items.size()));
                    PopulateOne(items[k], store, p, rep, nullptr);
                    ++c.repopulated;
                }
                if (!items.empty()) RefreshRavTrackNames(nullptr);  // 13-17 N3, as a Populate
            }
            SyncRun          run;
            run.scope.insert(request.scope.begin(), request.scope.end());
            for (const auto& kv : request.overwrite) run.overwrite[GuidKey(kv.first)] = kv.second;
            run.counts = &c;
            PopOpts o;
            o.sync = &run;
            o.report_replaced = false;
            o.only_populated = true;
            o.carry = &carried;
            std::vector<MediaItem*> items;
            for (const std::string& g : request.owners)
                if (MediaItem* it = FindItemByGuid(g))
                    if (RavTakeOf(it) && !again.count(GuidKey(g))) items.push_back(it);
            items = ByPosition(items);
            for (size_t k = 0; k < items.size(); ++k) {
                busy.Item(static_cast<int>(k), static_cast<int>(items.size()));
                PopulateOne(items[k], store, o, rep, nullptr);
            }
        }
        UpdateArrange();
    } catch (...) {
        AddLine(rep, ReportKind::Blocked, "Internal error: the gesture stopped part way (Ctrl+Z undoes it)", "", "", 0.0);
    }
    return rep;
}

SoundReport ReshuffleItems(const std::vector<MediaItem*>& items)
{
    SoundReport rep;
    try {
        const std::vector<MediaItem*> targets = ByPosition(PopulateTargets(items));
        if (targets.empty()) return rep;
        const SoundStore store = GetSoundStore(nullptr);
        {
            SoundUndoScope undo(nullptr, "RAV: Reshuffle sounds");
            ItemIndexScope index;
            BusyScope      busy("Reshuffling sounds");  // 13-59
            PopOpts o;
            o.only_populated = true;
            o.report_replaced = false;
            std::map<std::string, std::vector<int>> carried;  // 13-68: avoid-repeat across the items
            o.carry = &carried;
            for (size_t k = 0; k < targets.size(); ++k) {
                MediaItem* it = targets[k];
                busy.Item(static_cast<int>(k), static_cast<int>(targets.size()));
                const uint64_t seed = RandomSeed();
                ModifyItemSounds(it, nullptr, [seed](ItemSounds& s) {
                    Reshuffle(s, seed);
                    return true;
                });
                PopulateOne(it, store, o, rep, nullptr);
            }
        }
        UpdateArrange();
    } catch (...) {
        AddLine(rep, ReportKind::Blocked, "Internal error: the gesture stopped part way (Ctrl+Z undoes it)", "", "", 0.0);
    }
    return rep;
}

SoundReport ReshuffleEvents(const std::vector<ReshuffleEvent>& events, const std::string& undo_desc, PopulateCounts* counts)
{
    SoundReport rep;
    try {
        std::vector<MediaItem*> items;
        for (const ReshuffleEvent& e : events)
            if (e.item && std::find(items.begin(), items.end(), e.item) == items.end()) items.push_back(e.item);
        items = ByPosition(items);
        if (items.empty()) return rep;
        const SoundStore store = GetSoundStore(nullptr);
        {
            SoundUndoScope undo(nullptr, undo_desc.c_str());
            ItemIndexScope index;
            std::map<std::string, std::vector<int>> carried;  // 13-68: avoid-repeat across the items
            for (MediaItem* it : items) {
                const uint64_t seed = RandomSeed();
                ModifyItemSounds(it, nullptr, [&](ItemSounds& s) {
                    s.seed = seed;
                    for (const ReshuffleEvent& e : events)
                        if (e.item == it)
                            if (EventSounds* ev = FindEventSounds(s, e.rid, e.t))
                                ev->draws.erase(std::remove_if(ev->draws.begin(), ev->draws.end(), [](const SoundDraw& d) { return !d.hand; }),
                                                ev->draws.end());
                    return true;
                });
                for (const ReshuffleEvent& e : events) {
                    if (e.item != it) continue;
                    PopOpts o;
                    o.only_populated = true;
                    o.report_replaced = false;
                    o.carry = &carried;
                    o.counts = counts;
                    o.rid = e.rid;
                    o.has_t = true;
                    o.t = e.t;
                    PopulateOne(it, store, o, rep, nullptr);
                }
            }
        }
        UpdateArrange();
    } catch (...) {
        AddLine(rep, ReportKind::Blocked, "Internal error: the gesture stopped part way (Ctrl+Z undoes it)", "", "", 0.0);
    }
    return rep;
}

SoundReport SetRuleLibrary(MediaItem* item, int block, std::string library_uid)
{
    SoundReport rep;
    try {
        if (!item || !RavTakeOf(item)) return rep;
        const SoundStore store = GetSoundStore(nullptr);
        {
            SoundUndoScope undo(nullptr, "RAV: Set rule library");
            ItemIndexScope index;
            EnsureRids(item);
            const std::string rid = RidOf(item, block);
            if (rid.empty()) return rep;
            LibState    state;
            std::string lib;
            LibraryArg(library_uid, &state, &lib);
            const std::vector<MediaItem*> members = WithPoolMembers({item});
            for (MediaItem* m : members)
                ModifyItemSounds(m, nullptr, [&](ItemSounds& s) { return SetRulesLibrary(s, {rid}, state, lib); });
            PopOpts o;
            o.only_populated = true;
            o.follow_assign = true;  // Resilience 2026-10-19: every event of the rules follows the assignment
            o.rid = rid;
            PopulateMembers(members, store, o, rep);
        }
        UpdateArrange();
    } catch (...) {
        AddLine(rep, ReportKind::Blocked, "Internal error: the gesture stopped part way (Ctrl+Z undoes it)", "", "", 0.0);
    }
    return rep;
}

SoundReport SetSoundValue(MediaItem* item, SoundValueLevel level, int block, double t, std::string group_uid,
                          std::string value_uid)
{
    SoundReport rep;
    try {
        if (!item || !RavTakeOf(item) || group_uid.empty()) return rep;
        const SoundStore store = GetSoundStore(nullptr);
        {
            SoundUndoScope undo(nullptr, "RAV: Set sound value");
            ItemIndexScope index;
            std::string rid;
            if (level != SoundValueLevel::Item) {
                EnsureRids(item);
                rid = RidOf(item, block);
                if (rid.empty()) return rep;
            }
            const std::vector<MediaItem*> members = WithPoolMembers({item});
            for (MediaItem* m : members)
                ModifyItemSounds(m, nullptr, [&](ItemSounds& s) {
                    switch (level) {
                    case SoundValueLevel::Item: return SetValueIn(s.item_values, group_uid, value_uid);
                    case SoundValueLevel::Rule: {
                        RuleSounds* r = FindRuleSounds(s, rid);
                        if (!r) {
                            if (value_uid.empty()) return false;
                            RuleSounds fresh;
                            fresh.rid = rid;
                            s.rules.push_back(fresh);
                            r = &s.rules.back();
                        }
                        return SetValueIn(r->values, group_uid, value_uid);
                    }
                    case SoundValueLevel::Event: {
                        EventSounds* e = FindEventSounds(s, rid, t);
                        if (!e) {
                            if (value_uid.empty()) return false;
                            EventSounds fresh;
                            fresh.rid = rid;
                            fresh.t = t;
                            s.events.push_back(fresh);
                            e = &s.events.back();
                        }
                        return SetValueIn(e->overrides, group_uid, value_uid);
                    }
                    }
                    return false;
                });
            PopOpts o;
            o.only_populated = true;
            o.follow_assign = true;  // Resilience 2026-10-19: every event of the rules follows the assignment
            o.rid = rid;
            o.has_t = level == SoundValueLevel::Event;
            o.t = t;
            PopulateMembers(members, store, o, rep);
        }
        UpdateArrange();
    } catch (...) {
        AddLine(rep, ReportKind::Blocked, "Internal error: the gesture stopped part way (Ctrl+Z undoes it)", "", "", 0.0);
    }
    return rep;
}

SoundReport SetAutoMarkerLibrary(MediaItem* item, std::vector<std::string> keys, std::string library_uid)
{
    SoundReport rep;
    try {
        if (keys.empty()) return rep;
        {
            SoundUndoScope undo(nullptr, "RAV: Set auto marker library");
            ItemIndexScope index;
            LibState       state;
            std::string    lib;
            LibraryArg(library_uid, &state, &lib);
            const SoundStore store = GetSoundStore(nullptr);
            EditAutoRules(item, keys, store,
                          [&](ItemSounds& s, const std::vector<std::string>& rids) { return SetRulesLibrary(s, rids, state, lib); },
                          rep);
        }
        UpdateArrange();
    } catch (...) {
        AddLine(rep, ReportKind::Blocked, "Internal error: the gesture stopped part way (Ctrl+Z undoes it)", "", "", 0.0);
    }
    return rep;
}

SoundReport SetAutoMarkerValue(MediaItem* item, std::vector<std::string> keys, std::string group_uid,
                               std::string value_uid)
{
    SoundReport rep;
    try {
        if (keys.empty() || group_uid.empty()) return rep;
        {
            SoundUndoScope undo(nullptr, "RAV: Set sound value");
            ItemIndexScope index;
            const SoundStore store = GetSoundStore(nullptr);
            EditAutoRules(item, keys, store,
                          [&](ItemSounds& s, const std::vector<std::string>& rids) {
                              return SetRulesValue(s, rids, group_uid, value_uid);
                          },
                          rep);
        }
        UpdateArrange();
    } catch (...) {
        AddLine(rep, ReportKind::Blocked, "Internal error: the gesture stopped part way (Ctrl+Z undoes it)", "", "", 0.0);
    }
    return rep;
}

// ---- 13-15b: several rules at once (the assignment window's Item scope) ---------------------------

namespace {

// The rids of `rids` plus those of the item's `blocks` (given an id first), each once. Inside the caller's undo scope.
std::vector<std::string> RulesRidsFor(MediaItem* item, std::vector<std::string> rids, const std::vector<int>& blocks)
{
    if (!blocks.empty()) {
        EnsureRids(item);
        for (int b : blocks) {
            const std::string rid = RidOf(item, b);
            if (!rid.empty()) rids.push_back(rid);
        }
    }
    std::vector<std::string> out;
    for (const std::string& r : rids)
        if (!r.empty() && std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
    return out;
}

// `edit` on each pool member's record, then their populated events of those rules re-placed.
void EditRules(MediaItem* item, const std::vector<std::string>& rids, const SoundStore& store,
               const std::function<bool(ItemSounds&)>& edit, SoundReport& rep)
{
    const std::vector<MediaItem*> members = WithPoolMembers({item});
    for (MediaItem* m : members) ModifyItemSounds(m, nullptr, edit);
    PopOpts o;
    o.only_populated = true;
    o.follow_assign = true;  // Resilience 2026-10-19: every event of the rules follows the assignment
    o.rids = rids;
    PopulateMembers(members, store, o, rep);
}

}  // namespace

SoundReport SetRulesAssignLibrary(MediaItem* item, std::vector<std::string> rids, LibState state, std::string library_uid,
                                  std::vector<int> blocks, bool* changed)
{
    SoundReport rep;
    if (changed) *changed = false;
    try {
        if (!item || !RavTakeOf(item) || (rids.empty() && blocks.empty())) return rep;
        {  // Review 13-15b: an edit that changes nothing makes no undo point.
            ItemSounds probe = ReadItemSounds(item);
            if (!SetRulesLibrary(probe, rids, state, library_uid) && (blocks.empty() || state == LibState::Inherit))
                return rep;
        }
        if (changed) *changed = true;
        const SoundStore store = GetSoundStore(nullptr);
        {
            SoundUndoScope undo(nullptr, "RAV: Set rule library");
            ItemIndexScope index;
            const std::vector<std::string> all = RulesRidsFor(item, std::move(rids), blocks);
            if (all.empty()) return rep;
            EditRules(item, all, store, [&](ItemSounds& s) { return SetRulesLibrary(s, all, state, library_uid); }, rep);
        }
        UpdateArrange();
    } catch (...) {
        AddLine(rep, ReportKind::Blocked, "Internal error: the gesture stopped part way (Ctrl+Z undoes it)", "", "", 0.0);
    }
    return rep;
}

SoundReport SetRulesAssignValue(MediaItem* item, std::vector<std::string> rids, std::string group_uid,
                                std::string value_uid, std::vector<int> blocks, bool* changed)
{
    SoundReport rep;
    if (changed) *changed = false;
    try {
        if (!item || !RavTakeOf(item) || group_uid.empty() || (rids.empty() && blocks.empty())) return rep;
        {  // Review 13-15b: an edit that changes nothing makes no undo point.
            ItemSounds probe = ReadItemSounds(item);
            if (!SetRulesValue(probe, rids, group_uid, value_uid) && (blocks.empty() || value_uid.empty())) return rep;
        }
        if (changed) *changed = true;
        const SoundStore store = GetSoundStore(nullptr);
        {
            SoundUndoScope undo(nullptr, "RAV: Set sound value");
            ItemIndexScope index;
            const std::vector<std::string> all = RulesRidsFor(item, std::move(rids), blocks);
            if (all.empty()) return rep;
            EditRules(item, all, store, [&](ItemSounds& s) { return SetRulesValue(s, all, group_uid, value_uid); }, rep);
        }
        UpdateArrange();
    } catch (...) {
        AddLine(rep, ReportKind::Blocked, "Internal error: the gesture stopped part way (Ctrl+Z undoes it)", "", "", 0.0);
    }
    return rep;
}

// 13-66: the staged Switch values of the assignment window, written in one undo point and re-placed in one pass.
// PlanCommit decides first which edits still apply (their item and rule must be where they were staged).
SoundReport CommitAssignValues(const std::vector<av::StagedEntry>& edits, bool* changed, av::CommitCounts* counts)
{
    SoundReport rep;
    if (changed) *changed = false;
    if (counts) *counts = av::CommitCounts();
    try {
        if (edits.empty()) return rep;
        const av::CommitPlan plan = av::PlanCommit(edits, [](const std::string& guid) {
            av::CommitItemRead r;
            MediaItem*         it = FindItemByGuid(guid);
            if (!it || !RavTakeOf(it)) return r;
            r.found = true;
            ItemRulesRead rr;
            if (ReadItemRules(it, &rr) && rr.valid) {
                r.valid = true;
                r.blocks = rr.rules.blocks;
            }
            return r;
        });
        if (counts) *counts = plan.counts;
        if (plan.counts.skipped > 0)
            AddLine(rep, ReportKind::Info, av::SkippedText(plan.counts.skipped), "", "", 0.0);
        const SoundStore before = GetSoundStore(nullptr);
        auto apply_store = [&](SoundStore& st) {
            bool c = false;
            for (const av::StagedEntry& e : plan.store_edits) {
                if (e.scope == av::StageScope::Project) {
                    if (SetAssignValueIn(st.project, {e.key}, e.group, e.value)) c = true;
                } else if (EditCharacter(st, e.character, [&](SoundCharacter& ch) {
                               return SetAssignValueIn(ch.assigns, {e.key}, e.group, e.value);
                           })) {
                    c = true;
                }
            }
            return c;
        };
        bool any = false;  // an edit that changes nothing makes no undo point
        if (!plan.store_edits.empty()) {
            SoundStore probe = before;
            if (apply_store(probe)) any = true;
        }
        for (const av::PlannedItem& pi : plan.items) {
            MediaItem* item = FindItemByGuid(pi.item_guid);
            if (!item) continue;
            ItemSounds    probe = ReadItemSounds(item);
            ItemRulesRead rr;
            const bool    have = ReadItemRules(item, &rr) && rr.valid;
            for (const av::PlannedEdit& e : pi.edits) {
                if (e.block < 0) {
                    if (SetValueIn(probe.item_values, e.group, e.value)) any = true;
                    continue;
                }
                if (!have || static_cast<size_t>(e.block) >= rr.rules.blocks.size()) continue;
                const std::string& rid = e.rid.empty() ? rr.rules.blocks[static_cast<size_t>(e.block)].rid : e.rid;
                if (rid.empty()) {
                    if (!e.value.empty()) any = true;
                } else if (SetRulesValue(probe, {rid}, e.group, e.value)) {
                    any = true;
                }
            }
        }
        if (!any) return rep;
        if (changed) *changed = true;
        {
            SoundUndoScope undo(nullptr, "RAV: Set sound values");
            ItemIndexScope index;
            ExtraRids      extra;
            for (const av::PlannedItem& pi : plan.items) {
                MediaItem* item = FindItemByGuid(pi.item_guid);
                if (!item) continue;
                if (pi.NeedsRids()) EnsureRids(item);
                struct Resolved {
                    std::string rid;  // "" = the Item values row
                    std::string group, value;
                };
                std::vector<Resolved> res;
                for (const av::PlannedEdit& e : pi.edits) {
                    if (e.block < 0) {
                        res.push_back({std::string(), e.group, e.value});
                        continue;
                    }
                    const std::string rid = e.rid.empty() ? RidOf(item, e.block) : e.rid;
                    if (!rid.empty()) res.push_back({rid, e.group, e.value});
                }
                if (res.empty()) continue;
                // A pool is one owner: every copy takes the edits, each rule re-places once.
                const std::vector<MediaItem*> members = WithPoolMembers({item});
                for (MediaItem* m : members) {
                    ModifyItemSounds(m, nullptr, [&](ItemSounds& s) {
                        bool c = false;
                        for (const Resolved& r : res) {
                            if (r.rid.empty()) c = SetValueIn(s.item_values, r.group, r.value) || c;
                            else c = SetRulesValue(s, {r.rid}, r.group, r.value) || c;
                        }
                        return c;
                    });
                    for (const Resolved& r : res) {
                        if (r.rid.empty()) extra.whole.insert(m);
                        else extra.rids[m].insert(r.rid);
                    }
                }
            }
            if (!plan.store_edits.empty()) ModifySoundStore(nullptr, nullptr, apply_store);
            ReplaceChangedRules(before, rep, &extra, !plan.store_edits.empty());
        }
        UpdateArrange();
    } catch (...) {
        AddLine(rep, ReportKind::Blocked, "Internal error: the gesture stopped part way (Ctrl+Z undoes it)", "", "", 0.0);
    }
    return rep;
}

std::vector<MediaItem*> ProjectRavItems()
{
    return AllRavItems();
}

int ResetStaleRules(const char* undo_desc)
{
    int            n = 0;
    std::unique_ptr<SoundUndoScope> undo;
    if (undo_desc) undo = std::make_unique<SoundUndoScope>(nullptr, undo_desc);
    const SoundStore store = GetSoundStore(nullptr);
    for (MediaItem* it : AllRavItems())
        ModifyItemSounds(it, nullptr, [&](ItemSounds& s) {
            const int k = ResetStaleRuleLibraries(s, store);
            n += k;
            return k > 0;
        });
    ModifySoundStore(nullptr, nullptr, [&](SoundStore& s) {
        const int k = ResetStaleAssignLibraries(s);
        n += k;
        return k > 0;
    });
    return n;
}

// ---- 13-15a: Project and Character assignments ---------------------------------------------------

SoundReport SetProjectAssignLibrary(std::vector<std::string> keys, LibState state, std::string library_uid)
{
    return StoreGesture("RAV: Set project sound default", [&](SoundStore& st) {
        return SetAssignLibraryIn(st.project, keys, state, library_uid);
    });
}

SoundReport SetProjectAssignValue(std::vector<std::string> keys, std::string group_uid, std::string value_uid)
{
    return StoreGesture("RAV: Set project sound value", [&](SoundStore& st) {
        return SetAssignValueIn(st.project, keys, group_uid, value_uid);
    });
}

SoundReport SetCharacterAssignLibrary(CharacterRef character, std::vector<std::string> keys, LibState state,
                                      std::string library_uid)
{
    return StoreGesture("RAV: Set character library", [&](SoundStore& st) {
        return EditCharacter(st, character, [&](SoundCharacter& c) {
            return SetAssignLibraryIn(c.assigns, keys, state, library_uid);
        });
    });
}

SoundReport SetCharacterAssignValue(CharacterRef character, std::vector<std::string> keys, std::string group_uid,
                                    std::string value_uid)
{
    return StoreGesture("RAV: Set character sound value", [&](SoundStore& st) {
        return EditCharacter(st, character, [&](SoundCharacter& c) {
            return SetAssignValueIn(c.assigns, keys, group_uid, value_uid);
        });
    });
}

SoundReport AdoptKit(std::string library_uid, std::string part, CharacterRef character, bool replace)
{
    return StoreGesture("RAV: Use library kit", [&](SoundStore& st) {
        const SoundLibrary* lib = FindLibrary(st, library_uid);
        if (!lib || !KitServes(*lib, part)) return false;
        if (character.uid.empty() && character.skeleton.empty()) return AdoptKitIn(st.project, part, library_uid, replace);
        return EditCharacter(st, character, [&](SoundCharacter& c) { return AdoptKitIn(c.assigns, part, library_uid, replace); });
    });
}

SoundReport RenameCharacter(std::string character_uid, std::string name)
{
    return StoreGesture("RAV: Rename character", [&](SoundStore& st) { return RenameCharacterIn(st, character_uid, name); });
}

SoundReport SplitCharacter(std::string character_uid)
{
    std::vector<ModelRef> models;
    {
        const SoundStore&     store = GetSoundStore(nullptr);
        const SoundCharacter* c = FindCharacterByUid(store, character_uid);
        if (!c) return {};
        for (const ProjectRig& pr : ProjectRigs())
            if (pr.skeleton == c->skeleton)
                for (const RigModelCount& m : pr.models)
                    models.push_back({m.model, m.files.empty() ? std::string() : SplitCharacterName(m.files.front())});
    }
    return StoreGesture("RAV: Split character",
                        [&](SoundStore& st) { return SplitCharacterIn(st, character_uid, models) > 0; });
}

SoundReport RenameCharacterRef(CharacterRef character, std::string name)
{
    return StoreGesture("RAV: Rename character", [&](SoundStore& st) {
        return EditCharacter(st, character, [&](SoundCharacter& c) { return RenameCharacterIn(st, c.uid, name); });
    });
}

SoundReport SplitRig(CharacterRef character)
{
    std::vector<ModelRef> models;
    {
        std::string skeleton = character.skeleton;
        if (skeleton.empty())
            if (const SoundCharacter* c = FindCharacterByUid(GetSoundStore(nullptr), character.uid)) skeleton = c->skeleton;
        if (skeleton.empty()) return {};
        for (const ProjectRig& pr : ProjectRigs())
            if (pr.skeleton == skeleton)
                for (const RigModelCount& m : pr.models)
                    models.push_back({m.model, m.files.empty() ? std::string() : SplitCharacterName(m.files.front())});
    }
    return StoreGesture("RAV: Split character", [&](SoundStore& st) {
        const size_t    had = st.characters.size();
        SoundCharacter* c = EnsureCharacter(st, character);
        if (!c) return false;
        const std::string uid = c->uid;  // (a split added below may move the vector)
        const bool        split = SplitCharacterIn(st, uid, models) > 0;
        if (!split && st.characters.size() > had) st.characters.erase(st.characters.begin() + static_cast<long>(had), st.characters.end());
        return split;
    });
}

SoundReport OfferKit(std::string library_uid, bool accept)
{
    return StoreGesture(accept ? "RAV: Use library kit" : "RAV: Set library kit", [&](SoundStore& st) {
        SoundLibrary* lib = FindLibrary(st, library_uid);
        if (!lib) return false;
        bool changed = false;
        if (accept)
            for (const std::string& part : std::vector<std::string>(lib->kit))
                if (AdoptKitIn(st.project, part, library_uid, false)) changed = true;
        if (!lib->kit_offered) {
            lib->kit_offered = true;
            changed = true;
        }
        return changed;
    });
}

SoundReport SetLibraryKit(std::string library_uid, std::vector<std::string> parts, int offered)
{
    return StoreGesture("RAV: Set library kit", [&](SoundStore& st) {
        SoundLibrary* lib = FindLibrary(st, library_uid);
        if (!lib) return false;
        bool changed = SetKitParts(*lib, parts);
        if (offered >= 0 && lib->kit_offered != (offered != 0)) {
            lib->kit_offered = offered != 0;
            changed = true;
        }
        return changed;
    });
}

int CountDeletedByHandEvents(const std::vector<MediaItem*>& items)
{
    int n = 0;
    try {
        for (MediaItem* it : PopulateTargets(items)) n += CountDeletedByHand(ReadItemSounds(it));
    } catch (...) {
    }
    return n;
}

int CountPopulateEvents(const std::vector<MediaItem*>& items)
{
    int n = 0;
    try {
        const SoundStore& store = GetSoundStore(nullptr);
        if (store.libraries.empty()) return 0;
        for (MediaItem* it : PopulateTargets(items)) {
            const ItemDetection det = DetectItem(it);
            if (det.status != ItemDetection::Status::Ok) continue;
            const ItemSounds rec = ReadItemSounds(it);
            const ItemRigInfo     rig = ItemRig(it);
            const SoundCharacter* ch = CharacterFor(store, rig.skeleton, rig.model);
            std::vector<char> ok(det.rules.blocks.size(), 0);
            for (size_t b = 0; b < det.rules.blocks.size(); ++b) {
                // 13-15: a rule with no id yet (it never had a sound of its own) inherits one: Populate gives it its id.
                if (!det.rules.blocks[b].enabled) continue;
                const EffectiveAssign ea = ResolveBlock(store, rec, ch, det.rules.blocks[b]);
                if (ea.library.empty()) continue;
                const SoundLibrary* lib = FindLibrary(store, ea.library);
                ok[b] = lib && !CheckLibrary(store, *lib).blocked ? 1 : 0;
            }
            std::vector<int> evb;
            for (const ShownEvent& e : BuildEventList(det.events, det.rules.events, det.rules.blocks.size())) {
                if (e.kind != ShownKind::Detected && e.kind != ShownKind::User) continue;
                if (e.block >= 0 && static_cast<size_t>(e.block) < det.rules.blocks.size() &&
                    EventSkippedByPopulate(FindEventSounds(rec, det.rules.blocks[static_cast<size_t>(e.block)].rid, e.t)))
                    continue;  // the same test as PopulateOne
                evb.push_back(e.block);
            }
            n += CountEvents(evb, ok);
        }
    } catch (...) {
    }
    return n;
}

}  // namespace rav

#endif  // _WIN32
