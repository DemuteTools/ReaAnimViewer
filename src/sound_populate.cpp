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
#include <functional>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <thread>

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
#include "sound_switch.h"
#include "sound_tracks.h"
#include "tagging_session.h"
#include "tagging_signal.h"

namespace rav {

namespace {

using SetMediaItemSelectedFn = void (*)(MediaItem* item, bool selected);
SetMediaItemSelectedFn f_set_selected = nullptr;

constexpr int kUnselectAllItems = 40289;  // Item: Unselect (clear selection of) all items

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
    }
    ItemIndexScope(const ItemIndexScope&) = delete;
    ItemIndexScope& operator=(const ItemIndexScope&) = delete;

private:
    std::map<std::string, MediaItem*>  map_;
    std::map<std::string, MediaTrack*> tracks_;
    MeasureCache                       measured_;
    bool                               owner_ = false;
};

// RemovePlacement, the item dropped from the gesture's map.
bool RemoveIndexed(const std::string& g)
{
    if (g_items) g_items->erase(GuidKey(g));
    return RemovePlacement(nullptr, g);
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
    bool        report_replaced = true;
    // 13-56 review: a looped item's new passes filled by the follow (PopulateLoopPassesNoUndo): only
    // the Place steps of the events that already have their sounds (PlanPassFill); nothing removed,
    // replaced or re-drawn, no orphan prune.
    bool        fill_passes = false;
    std::string rid;                      // "" = every rule
    std::vector<std::string> rids;        // 13-8 fix: when not empty, only these rules (one detection for all)
    bool        has_t = false;
    double      t = 0.0;
    // 13-19: told before each of the item's events (handled so far, the item's count). Optional.
    std::function<void(int, int)> on_event;
};

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

// The request for one resolved sound (P11). `file` = a File Sound's target (PrepareFileSound). 13-20 /
// 13-23 / 13-25: `data` = the Sound's data to place (its syncs measured, PlacedDataFor; null = its
// own): a sliced, Whole-item or File variation is placed as the pieces of its part (PieceApplyFor).
PlaceRequest RequestFor(MediaItem* owner, const std::string& rid, double event_t, double project_t,
                        const SoundNode& n, const ResolvedSound& rs, const FileSoundTarget* file = nullptr,
                        const SoundData* data = nullptr)
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
    if (n.sound.source.kind == SoundSourceKind::File && file && !file->path.empty()) {
        // Antho 2026-10-12 (13-11): straight from the file, on its Lands-in layer track.
        req.file_path = file->path;
        req.track = file->track;
        req.slice = FileSliceFor(sd, v);
        req.fades = FileFadeApplyFor(sd, v);
        req.pieces = PieceApplyFor(sd, v, rs.part_value, true);
    } else if (n.sound.source.kind == SoundSourceKind::File && file && !file->item_guid.empty()) {
        req.source.kind = SoundSourceKind::Item;  // an older build's imported item: copied as before
        req.source.item_guid = file->item_guid;
    }
    if (req.source.kind == SoundSourceKind::Item && v >= 0 && v < static_cast<int>(n.sound.frames.size())) {
        if (sd.mode == DetectMode::Takes) req.variation = v;
        else if (sd.mode == DetectMode::TakeMarkers || sd.mode == DetectMode::Silence)
            req.slice = SliceApplyFor(sd.frames[static_cast<size_t>(v)]);
        req.fades = FadeApplyFor(sd, v);  // Antho 2026-10-12: fades on every placed variation
        // 13-20 / 13-23 / 13-25 (D7): the part's pieces, each from the sync point (a folder pile and
        // Takes ignore them: PlaceSounds).
        if (sd.mode != DetectMode::Takes) req.pieces = PieceApplyFor(sd, v, rs.part_value, false);
    }
    req.part = rs.part_value;
    req.gain_db = rs.gain_db;  // ResolveEvent already added the frame's dB
    // 13-29 / 13-41: placed as the audition plays it (no take FX, pan, playrate / pitch of the source item),
    // forced to mono as the Sound says.
    req.as_auditioned = true;
    req.chan_mode = ChanModeFor(sd.mono);
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

// One item: prune, draw, place / replace / remove, write its record. `events` counts the events
// handled.
void PopulateOne(MediaItem* item, const SoundStore& store, const PopOpts& o, SoundReport& rep, int* events)
{
    const ItemDetection det = DetectItem(item);
    if (det.status != ItemDetection::Status::Ok) return;
    const std::string guid = ItemGuidOf(item);
    const std::vector<Block>& blocks = det.rules.blocks;
    ItemSounds rec = ReadItemSounds(item);
    // 13-15a: each rule's library and values come from the layers (rule, Character, Project).
    const ItemRigInfo     rig = ItemRig(item);
    const SoundCharacter* ch = CharacterFor(store, rig.skeleton, rig.model);
    std::map<std::string, std::pair<std::string, std::string>> part_of;  // rid -> body part, type
    std::map<std::string, std::string> role_of;  // 13-23 (D1): rid -> its events' role key
    for (const Block& b : blocks)
        if (!b.rid.empty()) {
            std::pair<std::string, std::string> pt;
            PartOfBlock(b, &pt.first, &pt.second);
            part_of[b.rid] = pt;
            role_of[b.rid] = EventRoleKey(b);
        }
    MeasureCache measured_here;  // 13-25: outside a gesture (PlacedDataFor)

    // P1: assignments of rules that no longer exist.
    std::set<std::string> live;
    for (const Block& b : blocks)
        if (!b.rid.empty()) live.insert(b.rid);
    const OrphanPrune pr = o.fill_passes ? OrphanPrune{} : PruneOrphanRules(rec, live);
    for (const std::string& g : pr.remove_guids) RemoveIndexed(g);
    for (const RuleSounds& r : pr.rules) {  // Antho 2026-10-12: removed and listed, by every gesture
        if (r.library.empty()) continue;  // 13-15a: inherit / None: no library was unassigned
        const SoundLibrary* lib = FindLibrary(store, r.library);
        AddLine(rep, ReportKind::Detached, RuleGoneText(lib ? lib->name : r.library), "", guid, 0.0);
    }

    if (rec.seed == 0) rec.seed = RandomSeed();
    PopulateDrawState st;
    st.seed = rec.seed;
    // 13-59: New variation per loop pass: each pass other than the first draws its own (its history
    // kept per pass, across the item's events in time order).
    const bool                               pass_draws = LoopPassDrawOf(store) == LoopPassDraw::New;
    std::map<long long, PopulateDrawState>   pass_states;
    std::set<std::string> blocked_rules;
    std::set<std::string> gone_rules;
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
            SoundTracks      st;
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
    for (const FollowEvent& e : sound_events) {
        if (o.on_event) o.on_event(handled, n_events);
        else BusyStep(handled, n_events);  // 13-59: the busy card of the gesture running this (if any)
        ++handled;
        const std::string& rid = e.rid;
        if (!e.enabled) continue;  // fix C 2026-10-12: a disabled rule's sounds are kept, muted by the follow
        if (!o.rid.empty() && rid != o.rid) continue;
        if (!o.rids.empty() && std::find(o.rids.begin(), o.rids.end(), rid) == o.rids.end()) continue;
        if (o.has_t && std::fabs(e.t - o.t) > kEventMatchSeconds) continue;
        EventSounds* ev = FindEventSounds(rec, rid, e.t);
        if (o.only_populated && (!ev || !EventIsPopulated(*ev))) continue;
        if (EventSkippedByPopulate(ev)) continue;  // attached / spread: left alone

        const auto            po = part_of.find(rid);
        const EffectiveAssign ea = po == part_of.end()
                                       ? EffectiveAssign()
                                       : ResolveAssignFor(store, rec, ch, rid, po->second.first, po->second.second, e.t);
        const SoundLibrary*   lib = !ea.library.empty() ? FindLibrary(store, ea.library) : nullptr;
        if (!lib && o.fill_passes) continue;  // 13-56 review: a fill never removes
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

        // P9: a recorded item no longer in the project was deleted by hand.
        MarkDeletedPlacements(*ev, [](const std::string& g) { return FindItemByGuid(g) != nullptr; });
        if (o.revive_deleted)
            ev->placed.erase(std::remove_if(ev->placed.begin(), ev->placed.end(),
                                            [](const SoundPlacement& p) { return p.own == Ownership::Deleted; }),
                             ev->placed.end());

        // P10: the draw.
        st.lib = lib;
        const std::vector<SoundDraw> prev = ev->draws;
        EventDraw ed;
        ed.rid = rid;
        ed.t = ev->t;
        ed.prev = &prev;
        const std::string lib_uid = lib->uid;
        auto is_missing = [&file_missing](const SoundNode& n) {
            if (n.sound.source.kind == SoundSourceKind::File) {  // 13-11: its file (no side effect here)
                auto it = file_missing.find(n.uid);
                if (it == file_missing.end()) it = file_missing.emplace(n.uid, FileSoundMissing(n)).first;
                return it->second;
            }
            return !FindItemByGuid(n.sound.source.item_guid);
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
            // 13-59: the loop passes' own picks are kept (those drawn below replace theirs).
            for (const SoundDraw& d : prev)
                if (ParsePassDrawKey(d.node, nullptr, nullptr)) draws.push_back(d);
            ev->draws = draws;
        }
        if (res.blocked) {
            AddLine(rep, ReportKind::Blocked, res.reason.empty() ? std::string("blocked") : res.reason, "", guid, e.t);
            continue;
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
                // pile and Takes are not counted (their members are the pile's). 13-25: the snap its anchor
                // piece gets (item s), compared too, so a sync point measured or moved since is placed again.
                int    pieces = 0;
                double sync = -1.0;
                if (n->sound.mode != DetectMode::Takes && s.variation >= 0) {
                    bool   pile = false;
                    double rate = 1.0;
                    if (n->sound.source.kind == SoundSourceKind::Item)
                        if (MediaItem* src = FindItemByGuid(n->sound.source.item_guid)) {
                            pile = IsFolderItem(src);
                            if (MediaItem_Take* tk = GetActiveTake(src)) rate = GetMediaItemTakeInfo_Value(tk, "D_PLAYRATE");
                        }
                    if (!pile) {
                        const SoundData& sd = PlacedDataFor(
                            lib_uid, *n, s.variation, [n]() { return SoundSourcePath(n->sound.source); }, measured_here);
                        const std::vector<PieceApply> pa = PieceApplyFor(sd, s.variation, s.part_value,
                                                                         n->sound.source.kind == SoundSourceKind::File);
                        pieces = static_cast<int>(pa.size());
                        if (!(rate > 0.0) || !std::isfinite(rate)) rate = 1.0;
                        for (const PieceApply& a : pa)
                            if (a.anchor) sync = a.slice.snap_offset / rate;
                    }
                }
                for (int p : on) {
                    DesiredSlot d;
                    d.pass = p;
                    d.node = n->uid;
                    d.variation = s.variation;
                    d.part = s.part_value;
                    d.pieces = pieces;
                    d.sync = sync;
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
            pd.prev = PassPrevDraws(prev, pd.id);
            pd.ed.rid = rid;
            pd.ed.t = ev->t;
            pd.ed.prev = &pd.prev;
            pd.res = ResolveEvent(store, *lib, layers, MakeDrawHooks(ps, pd.ed, is_missing));
            if (pd.res.blocked) continue;
            MergePassDraws(ev->draws, pd.id, pd.ed.out);
            add_desired(pd.res, pd.ed, pd.on);
        }
        std::vector<ExistingSlot> existing;
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
                }
            existing.push_back(x);
        }

        std::set<int>               erase;
        std::vector<SoundPlacement> added;
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
        for (const SlotStep& step : o.fill_passes ? PlanPassFill(desired, existing) : PlanEventSlots(desired, existing)) {
            if (step.act == SlotAction::Keep) {
                if (o.counts) ++o.counts->kept;
                // 13-16 (B7c): a kept item still on its RAV track that now overlaps (a follow
                // move) goes to a free spill child of the same track.
                for (int i : step.existing)
                    if (MediaItem* it = FindItemByGuid(ev->placed[static_cast<size_t>(i)].item_guid)) FixLaneOverlap(nullptr, it);
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
            if (n->sound.source.kind == SoundSourceKind::File) {  // readied once per gesture, when first placed
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
                [file, n]() { return file && !file->path.empty() ? file->path : SoundSourcePath(n->sound.source); },
                measured_here);
            PlaceRequest  req = RequestFor(item, rid, ev->t, pt, *n, s, file, &placed_data);
            if (switch_track) {  // 13-13: every copy on its Switch-named track; 13-16: or its layer track
                req.track = switch_track;
                req.track_mark = track_mark;
            } else if (req.track && file && req.track == file->track) {
                req.track_mark = TrackMarkOf(req.track);  // 13-11's "layer N" track: RAV's, it spills too (B6)
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
                r = ReplacePlacement(nullptr, old.item_guid, req);
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
            if (o.counts) ++(step.act == SlotAction::Place ? o.counts->placed : o.counts->replaced);  // per sound, not per pile item
            if (g_items)
                for (size_t k = 0; k < r.item_guids.size() && k < r.items.size(); ++k)
                    (*g_items)[GuidKey(r.item_guids[k])] = r.items[k];
            CheckOwners(r.item_guids, guid, rep, n->uid, e.t);
            for (const std::string& g : r.item_guids) {
                SoundPlacement p;
                p.item_guid = g;
                p.node = n->uid;
                p.own = Ownership::Populated;
                p.offset_s = req.offset_s;
                p.variation = s.variation;
                added.push_back(p);
            }
        }
        std::vector<SoundPlacement> placed;
        for (size_t i = 0; i < ev->placed.size(); ++i)
            if (!erase.count(static_cast<int>(i))) placed.push_back(ev->placed[i]);
        placed.insert(placed.end(), added.begin(), added.end());
        ev->placed = placed;
    }

    ModifyItemSounds(item, nullptr, [&](ItemSounds& s) {
        s = rec;
        return true;
    });
}

// 13-59: PopulateOne over the members of an item's pool, under the busy card ("Updating sounds").
void PopulateMembers(const std::vector<MediaItem*>& members, const SoundStore& store, const PopOpts& o, SoundReport& rep)
{
    BusyScope busy("Updating sounds");
    for (size_t k = 0; k < members.size(); ++k) {
        busy.Item(static_cast<int>(k), static_cast<int>(members.size()));
        PopulateOne(members[k], store, o, rep, nullptr);
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

// After a store edit (13-15a): the populated events of every RAV item whose effective library or
// values changed (ChangedRids) are re-placed. Inside the caller's undo scope and item index.
void ReplaceChangedRules(const SoundStore& before, SoundReport& rep)
{
    const SoundStore after = GetSoundStore(nullptr);
    const std::vector<MediaItem*> all = AllRavItems();
    BusyScope busy("Updating sounds");  // 13-59: a library or value change re-placing many items
    for (size_t k = 0; k < all.size(); ++k) {
        MediaItem* it = all[k];
        busy.Item(static_cast<int>(k), static_cast<int>(all.size()));
        ItemRulesRead rr;
        if (!ReadItemRules(it, &rr) || !rr.valid) continue;
        const ItemRigInfo rig = ItemRig(it);
        const std::vector<std::string> rids =
            ChangedRids(before, after, ReadItemSounds(it), rr.rules.blocks, rig.skeleton, rig.model);
        if (rids.empty()) continue;
        PopOpts o;
        o.only_populated = true;
        o.rids = rids;
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
                                 bool revive_deleted, const PopulateProgressFn& progress)
{
    SoundReport rep;
    if (events) *events = 0;
    if (counts) *counts = PopulateCounts();
    try {
        const std::vector<MediaItem*> targets = PopulateTargets(items);
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
        BusyScope   busy("Filling loop passes");  // 13-59: a long stretch fills many passes
        for (size_t k = 0; k < items.size(); ++k) {
            MediaItem* it = items[k];
            busy.Item(static_cast<int>(k), static_cast<int>(items.size()));
            if (it && RavTakeOf(it)) PopulateOne(it, store, o, quiet, nullptr);
        }
        RefreshRavTrackNames(nullptr);  // 13-17 N3, as Populate
        UpdateArrange();
    } catch (...) {
        AddLine(rep, ReportKind::Blocked, "Internal error: the loop passes were not all filled", "", "", 0.0);
    }
    return rep;
}

SoundReport ReshuffleItems(const std::vector<MediaItem*>& items)
{
    SoundReport rep;
    try {
        const std::vector<MediaItem*> targets = PopulateTargets(items);
        if (targets.empty()) return rep;
        const SoundStore store = GetSoundStore(nullptr);
        {
            SoundUndoScope undo(nullptr, "RAV: Reshuffle sounds");
            ItemIndexScope index;
            BusyScope      busy("Reshuffling sounds");  // 13-59
            PopOpts o;
            o.only_populated = true;
            o.report_replaced = false;
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

std::vector<MediaItem*> ProjectRavItems()
{
    return AllRavItems();
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
