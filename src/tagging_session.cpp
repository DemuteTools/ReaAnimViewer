// SPDX-License-Identifier: MIT
//
// See tagging_session.h.

#include "tagging_session.h"

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <map>
#include <memory>

#include "asset_cache.h"
#include "asset_loader.h"
#include "bone_sampling.h"
#include "console_log.h"
#include "footstep_measure.h"  // kDetectRateHz
#include "item_rules.h"
#include "motion_physics.h"
#include "pcm_source_anim.h"
#include "role_map_store.h"
#include "tag_markers.h"  // the marker option (markers up to date)

namespace rav {
namespace {

constexpr double kRereadSeconds = 1.0;     // the record and the selection, without a state change
constexpr double kStampCheckSeconds = 1.0; // the file on disk (a re-export)

double Now()
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point t0 = clock::now();
    return std::chrono::duration<double>(clock::now() - t0).count();
}

struct SampleKey {
    std::string      path;
    AssetFileStamp   stamp;
    std::vector<int> bones;
    bool operator==(const SampleKey& o) const
    {
        return path == o.path && stamp == o.stamp && bones == o.bones;
    }
};

struct Session {
    TaggingModel model;
    // The record as last read (raw text), to see a change.
    std::string raw;
    bool        raw_present = false;
    bool        read_done = false;  // the record was read for this item and these bones
    int         state_count = -1;
    double      reread_at = -1.0;
    // The parse of the item's file, held so the cache's trim never makes us re-parse it.
    std::string                     asset_path;
    AssetFileStamp                  asset_stamp{};
    double                          stamp_checked_at = -1.0;
    std::shared_ptr<const CpuAsset> asset;
    // The tracks sampled for the current bone set.
    bool                   have_tracks = false;
    SampleKey              sample_key;
    std::vector<BoneTrack> tracks;
    // Story 10-8b: the physics role tracks (indexed by Role) for the auto blocks, sampled once per
    // (file, version, the physics roles' bones), and the analyses run on them (one per sensitivity set).
    bool                      have_role_tracks = false;
    SampleKey                 role_key;
    std::vector<BoneTrack>    role_tracks;
    std::vector<AutoAnalysis> auto_cache;
    // The rules shown: the preview while a drag runs.
    bool      previewing = false;
    ItemRules preview;
    bool      preview_event = false;  // the preview is an event's time (its commit auto-applies)
    bool      detect_dirty = true;
    std::string last_error;
    // The selection's footer.
    double sel_at = -1.0;
    int    sel_state_count = -1;
    // Bone names and parents per file, for the selection's role check (parsed once per session).
    std::map<std::string, SkeletonBones> names_by_path;
};

Session g;

const SkeletonBones& BoneNamesOf(const std::string& path)
{
    auto it = g.names_by_path.find(path);
    if (it != g.names_by_path.end()) return it->second;
    SkeletonBones sk;
    if (path == g.asset_path && g.asset) {
        sk = SkeletonBonesOf(g.asset->skeleton);
    } else if (const std::shared_ptr<const CpuAsset> a = AcquireCpuAsset(path)) {
        sk = SkeletonBonesOf(a->skeleton);
    }
    if (g.names_by_path.size() > 64) g.names_by_path.erase(g.names_by_path.begin());  // one out, not all
    return g.names_by_path[path] = std::move(sk);
}

// How many bone references the blocks read that have no bone on this skeleton, and their names.
// A joint angle whose joint has no parent or child counts too ("<name> (not a joint)").
int CountMissing(const std::vector<Block>& blocks, const std::vector<int>& role_to_bone,
                 const std::map<std::string, int>& custom_role_to_bone, const std::vector<std::string>& names,
                 const std::vector<int>& parents, std::string* missing)
{
    const std::vector<std::string> list = MissingBoneRefs(blocks, role_to_bone, custom_role_to_bone, names, parents);
    std::string all;
    for (const std::string& nm : list) all += (all.empty() ? "" : ", ") + nm;
    if (missing) *missing = all;
    return static_cast<int>(list.size());
}

// The preset field: the copy's id and versions, the preset as it is on disk now (its
// current name, version and the state derived from it).
void ReadPresetFields()
{
    TaggingModel& m = g.model;
    m.has_preset = m.has_record && m.rules.has_preset;
    const PresetCopy& c = m.rules.preset_copy;
    m.preset_id = m.has_preset ? c.id : "";
    m.preset_version = m.has_preset ? c.version : 0;
    m.kept_version = m.has_preset ? c.kept_version : 0;
    m.preset_factory = m.has_preset && IsFactoryPresetId(c.id);
    PresetInfo info;
    const bool found = m.has_preset && FindPreset(RulesResourceRoot(), c.id, &info);
    m.preset_gone = m.has_preset && !found;
    m.disk_version = found ? info.version : 0;
    m.preset_name = !m.has_preset ? "" : (found && !info.name.empty()) ? info.name : c.name;
    m.preset_state = m.has_preset ? PresetStateOf(m.rules, found ? &info : nullptr) : PresetState::Unknown;
}

// 10-8b fb-1: the rules a missing role skips on the current item's skeleton (none when the file
// did not load: nothing binds then, as before).
BlockSkips SessionSkips(const std::vector<Block>& blocks)
{
    const TaggingModel& m = g.model;
    if (!m.file_loaded) {
        BlockSkips none;
        none.skipped.assign(blocks.size(), 0);
        none.missing.assign(blocks.size(), std::string());
        return none;
    }
    return SkippedBlocks(blocks, m.role_to_bone, m.custom_role_to_bone, m.bone_names, m.bone_parents, IsActiveAutoBlock);
}

// The model's copy of the skips (the rules shown).
void StoreSkips(const BlockSkips& k)
{
    TaggingModel& m = g.model;
    m.skipped = k.skipped;
    m.skipped_missing = k.missing;
    m.skipped_count = k.count;
    m.nothing_runs = k.count > 0 && k.nothing_runs;
}

// The item's record, preset state and skeleton binding.
void ReadItem()
{
    TaggingModel& m = g.model;
    ItemRulesRead rd;
    ReadItemRules(m.item, &rd);
    g.raw = rd.raw;
    g.raw_present = rd.present;
    g.read_done = true;
    m.unreadable = rd.present && !rd.valid;
    m.has_record = rd.present && rd.valid;
    m.rules = m.has_record ? rd.rules : ItemRules{};
    // 10-6: a linked item with no record holds its pool's tagging: shown as its rules (read
    // through; nothing is written until a gesture, which writes it with them).
    if (!rd.present && ItemPoolContent(m.item, &m.rules)) m.has_record = true;
    ReadPresetFields();
    // Story 10-3e: the user's roles and the role keys the rules read (all of them, off rules
    // too: the window lists them), mapped on this skeleton; the display names follow roles.txt.
    RoleMapping rm = ItemRoleMapping(m.bone_names, m.rules.blocks);
    m.custom_roles = std::move(rm.roles);
    m.rule_role_keys = CustomRoleKeysUsed(m.rules.blocks);
    if (m.bone_names.empty()) {
        m.role_to_bone.clear();
        m.custom_role_to_bone.clear();
    } else {
        m.role_to_bone = std::move(rm.builtin);
        m.custom_role_to_bone = std::move(rm.custom);
    }
    m.roles_status = GetRoleMapStatus(RulesResourceRoot());
    m.missing.clear();
    m.missing_count = m.file_loaded ? CountMissing(RunnableBlocks(m.rules.blocks), m.role_to_bone, m.custom_role_to_bone,
                                                   m.bone_names, m.bone_parents, &m.missing)
                                    : 0;
    StoreSkips(SessionSkips(m.rules.blocks));
    g.detect_dirty = true;
}

// The item's file: parsed once per version on disk.
void LoadFile(double now)
{
    TaggingModel& m = g.model;
    const bool path_changed = m.path != g.asset_path;
    if (!path_changed && now - g.stamp_checked_at < kStampCheckSeconds) return;
    g.stamp_checked_at = now;
    const AssetFileStamp stamp = ReadAssetFileStamp(m.path);
    if (!path_changed && stamp == g.asset_stamp) return;
    g.asset_path = m.path;
    g.asset_stamp = stamp;
    g.asset = m.path.empty() ? nullptr : AcquireCpuAsset(m.path);
    g.have_tracks = false;
    g.have_role_tracks = false;
    g.auto_cache.clear();
    g.names_by_path.erase(m.path);
    m.bone_names.clear();
    m.bone_parents.clear();
    if (g.asset) {
        SkeletonBones sk = SkeletonBonesOf(g.asset->skeleton);
        m.bone_names = std::move(sk.names);
        m.bone_parents = std::move(sk.parents);
    }
    m.file_loaded = g.asset && !g.asset->animations.empty() && !m.bone_names.empty();
    m.map.clip_len = (g.asset && !g.asset->animations.empty()) ? g.asset->animations[0].duration : 0.0;
    g.read_done = false;  // the binding depends on the bones: read again
    g.reread_at = -1.0;
}

void ReadItemTiming()
{
    TaggingModel& m = g.model;
    MediaItem_Take* take = RavTakeOf(m.item);
    m.map = ItemClipMapOf(m.item, m.map.clip_len);
    const char* nm = take ? GetTakeName(take) : nullptr;
    m.item_name = (nm && nm[0]) ? nm : "(unnamed item)";
}

// The event list over the trace, what Apply would write, and whether it is written.
void BuildEvents()
{
    TaggingModel& m = g.model;
    m.events.clear();
    m.planned.clear();
    m.markers_up_to_date = false;
    m.has_previews = HasPreviews(m.rules);
    if (!m.detected) return;
    const ItemRules& shown = TaggingShownRules();
    m.events = BuildEventList(m.trace.events, shown.events, shown.blocks.size());
    m.planned = PlanMarkers(m.events, shown.blocks, m.map);
    m.markers_up_to_date = MarkersUpToDate(m.rules, m.planned, GetTaggingMarkerMode());
    m.has_previews = HasPreviews(m.rules);
}

// Binds `rules` on the skeleton and turns them into track indices; samples the bones when
// the bone set (or the file) changed. False when they do not bind or cannot be sampled.
// 10-8b fb-1: a rule a missing role skips is bound as an off rule (no bone, never fires).
bool BindAndSample(const ItemRules& rules, std::vector<Block>* bound_out)
{
    TaggingModel& m = g.model;
    if (!m.file_loaded || !g.asset) return false;
    std::vector<Block> bound = RunnableBlocks(rules.blocks, SessionSkips(rules.blocks).skipped);
    std::string why;
    if (!BindBoneRefs(bound, m.role_to_bone, m.custom_role_to_bone, m.bone_names, m.bone_parents, &why)) return false;
    std::vector<int> bones;
    RemapToTracks(bound, &bones);
    SampleKey key{g.asset_path, g.asset_stamp, bones};
    if (!g.have_tracks || !(key == g.sample_key)) {
        g.tracks = bones.empty() ? std::vector<BoneTrack>{} : SampleBoneTracks(*g.asset, bones, kDetectRateHz);
        g.sample_key = std::move(key);
        g.have_tracks = true;
        m.sample_error = (!bones.empty() && g.tracks.size() != bones.size()) ? "The animation could not be sampled." : "";
    }
    if (!bones.empty() && g.tracks.size() != bones.size()) return false;
    if (bound_out) *bound_out = std::move(bound);
    return true;
}

// Story 10-8b: the key of the physics role tracks: the file and the bone of each physics role
// (-1 = none), in PhysicsRoles() order.
SampleKey RoleTrackKey(const std::string& path, const AssetFileStamp& stamp, const std::vector<int>& role_to_bone)
{
    SampleKey key{path, stamp, {}};
    for (Role r : PhysicsRoles()) {
        const size_t i = static_cast<size_t>(r);
        key.bones.push_back(i < role_to_bone.size() ? role_to_bone[i] : -1);
    }
    return key;
}

std::vector<BoneTrack> SampleRoleTracks(const CpuAsset& asset, const std::vector<int>& role_to_bone)
{
    return PhysicsRoleTracks(role_to_bone,
                             [&](const std::vector<int>& bones) { return SampleBoneTracks(asset, bones, kDetectRateHz); });
}

// The session's role tracks for the current file and mapping (sampled when either changed; the
// analyses and story 10-8h's search results go with the old ones). False: no file.
bool EnsureRoleTracks()
{
    TaggingModel& m = g.model;
    if (!g.asset) return false;
    SampleKey key = RoleTrackKey(g.asset_path, g.asset_stamp, m.role_to_bone);
    if (!g.have_role_tracks || !(key == g.role_key)) {
        g.role_tracks = SampleRoleTracks(*g.asset, m.role_to_bone);
        g.role_key = std::move(key);
        g.have_role_tracks = true;
        g.auto_cache.clear();
        m.auto_searched.clear();
        m.auto_none.clear();
    }
    return true;
}

// The current item's auto events for `blocks` (the session's role tracks, sampled when the file
// or the mapping changed); m.auto_error says why they found nothing.
std::vector<Event> SessionAutoEvents(const std::vector<Block>& blocks)
{
    TaggingModel& m = g.model;
    m.auto_error.clear();
    if (!HasActiveAutoBlocks(blocks) || !EnsureRoleTracks()) return {};
    std::vector<Event> ev = DetectAutoEvents(blocks, g.role_tracks, &g.auto_cache);
    for (const PhysicsParams& p : AutoParamSets(blocks))
        for (const AutoAnalysis& a : g.auto_cache)
            if (SameAutoAnalysis(a.params, p) && !a.analysis.ok && m.auto_error.empty()) m.auto_error = a.analysis.error;
    return ev;
}

// The rules' detections and the auto blocks' events in one list, by time (then block).
void MergeEvents(std::vector<Event>& into, std::vector<Event> more)
{
    if (more.empty()) return;
    into.insert(into.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
    std::stable_sort(into.begin(), into.end(), [](const Event& a, const Event& b) {
        return a.time_s < b.time_s || (a.time_s == b.time_s && a.block < b.block);
    });
}

void RunDetection()
{
    TaggingModel& m = g.model;
    g.detect_dirty = false;
    m.detected = false;
    m.trace = DetectionTrace{};
    m.events.clear();
    m.planned.clear();
    m.markers_up_to_date = false;
    m.has_previews = HasPreviews(m.rules);
    m.auto_error.clear();
    const ItemRules& shown = TaggingShownRules();
    // 10-8b fb-1: a rule a missing role skips does not run; the others and the auto blocks do.
    // Nothing left to run: no detection (the strip says so), as before.
    const BlockSkips skips = SessionSkips(shown.blocks);
    StoreSkips(skips);
    if (m.nothing_runs) return;
    std::vector<Block> bound;
    if (!BindAndSample(shown, &bound)) return;
    m.trace = DetectTrace(bound, g.tracks, shown.options);
    // Story 10-8b: the auto blocks' events (their trace rows did not run: no curve, events only).
    MergeEvents(m.trace.events, SessionAutoEvents(shown.blocks));
    // 10-8b fb-1: the skipped rules and the auto blocks whose part has no bone keep their last
    // Commit's markers (the record's Detected entries stand in for their detections).
    const std::vector<char> keep =
        KeepUnrunnableEvents(m.trace.events, shown.events, shown.blocks, skips.skipped, m.role_to_bone);
    // Each row's events (a rule's from DetectTrace already; the auto and kept rows' from the list).
    for (size_t b = 0; b < m.trace.blocks.size(); ++b) {
        const bool from_list = (b < shown.blocks.size() && IsAutoBlock(shown.blocks[b])) || (b < keep.size() && keep[b]);
        if (!from_list) continue;
        std::vector<Event>& row = m.trace.blocks[b].events;
        row.clear();
        for (const Event& e : m.trace.events)
            if (e.block == static_cast<int>(b)) row.push_back(e);
    }
    m.detected = true;
    BuildEvents();
}

// 10-6: the current item's pool: how many other items share its tagging, and whether it was made
// unique. At the selection's cadence (a pool scan reads the records of the file's items).
void RefreshPool()
{
    TaggingModel& m = g.model;
    m.pool_others = 0;
    m.pool_unique = false;
    if (!m.item || m.unreadable) return;
    m.pool_others = static_cast<int>(PoolMembersOf(m.item).size());
    m.pool_unique = m.has_record && !m.rules.pool_id.empty();
}

// The footer's counts: the selected items, those without rules, those whose roles miss. 10-6:
// over the selected items and the other copies of their pools (Commit and Cancel act on those).
void RefreshSelection()
{
    TaggingModel& m = g.model;
    m.sel_count = m.sel_linked = m.sel_without_rules = m.sel_roles_skipped = m.sel_cancellable = m.sel_to_commit = 0;
    const int n = CountSelectedMediaItems(nullptr);
    m.sel_count = n;
    std::vector<MediaItem*> selected;
    for (int i = 0; i < n; ++i) selected.push_back(GetSelectedMediaItem(nullptr, i));
    const std::vector<MediaItem*> all = WithPoolMembers(selected);
    m.sel_linked = std::max(0, static_cast<int>(all.size()) - n);
    for (MediaItem* it : all) {
        ItemRulesRead rd;
        bool          readable = it && ReadItemRules(it, &rd) && rd.present && rd.valid;
        // 10-4 fb-4: Cancel acts on an item with rules, a Cancel snapshot or previews.
        if (readable && (!rd.rules.blocks.empty() || HasPreviews(rd.rules) || ReadCommittedSnapshot(it, nullptr)))
            ++m.sel_cancellable;
        // 10-6: a linked item with no record holds its pool's tagging (Commit gives it that first).
        if (it && !rd.present && ItemPoolContent(it, &rd.rules)) readable = true;
        if (!readable || rd.rules.blocks.empty()) {
            ++m.sel_without_rules;
            continue;
        }
        const std::string path = AnimPathOf(RavTakeOf(it));
        const SkeletonBones& sk = BoneNamesOf(path);
        const std::vector<std::string>& names = sk.names;
        // 10-8b fb-1: skipped for roles only when nothing can run (a rule with an unmapped role is
        // skipped alone; the item's other rules and auto blocks are committed).
        if (names.empty()) {
            ++m.sel_roles_skipped;
            continue;
        }
        const RoleMapping rm = ItemRoleMapping(names, rd.rules.blocks);
        const BlockSkips  skips = SkippedBlocks(rd.rules.blocks, rm.builtin, rm.custom, names, sk.parents, IsActiveAutoBlock);
        if (skips.count > 0 && skips.nothing_runs) {
            ++m.sel_roles_skipped;
            continue;
        }
        // Something to commit: never committed, previews pending, or committed under another
        // option (every gesture and option change rewrites the previews, so no preview on a
        // committed item means its result is the committed one).
        if (!rd.rules.has_applied || HasPreviews(rd.rules) || rd.rules.applied.mode != GetTaggingMarkerMode())
            ++m.sel_to_commit;
    }
}

}  // namespace

void TaggingSessionFrame(MediaItem* item, const std::string& path)
{
    try {
        TaggingModel& m = g.model;
        const double now = Now();
        if (item && !ValidatePtr2(nullptr, item, "MediaItem*")) item = nullptr;
        const bool item_changed = item != m.item || path != m.path;
        if (item_changed) {
            g.previewing = false;  // a preview belongs to the item it began on
            m.item = item;
            m.path = item ? path : "";
            g.read_done = false;
            g.reread_at = -1.0;
            g.detect_dirty = true;
            m.auto_searched.clear();  // story 10-8h: the search results belong to the item
            m.auto_none.clear();
            if (!item) {
                // The file goes too: the next item (even of the same file) loads it again.
                g.asset_path.clear();
                g.asset.reset();
                g.asset_stamp = AssetFileStamp{};
                g.have_tracks = false;
                g.have_role_tracks = false;
                g.auto_cache.clear();
                g.stamp_checked_at = -1.0;
                const int sc = m.sel_count, snr = m.sel_without_rules, srs = m.sel_roles_skipped;
                const int stc = m.sel_to_commit, sca = m.sel_cancellable, sln = m.sel_linked;
                m = TaggingModel{};
                m.sel_count = sc;
                m.sel_linked = sln;
                m.sel_without_rules = snr;
                m.sel_roles_skipped = srs;
                m.sel_to_commit = stc;
                m.sel_cancellable = sca;
            }
        }
        const int count = GetProjectStateChangeCount(nullptr);
        if (m.item) {
            LoadFile(now);
            ReadItemTiming();
            if (item_changed || count != g.state_count || g.reread_at < 0.0 || now >= g.reread_at) {
                ItemRulesRead peek;
                ReadItemRules(m.item, &peek);
                // Read again (and re-bind) only when the record changed: a new item, an
                // undo, another script, or the file's bones.
                if (!g.read_done || peek.raw != g.raw || peek.present != g.raw_present) ReadItem();
                else ReadPresetFields();  // story 10-3b: the preset file may have changed on disk
                g.reread_at = now + kRereadSeconds;
            }
        }
        m.previewing = g.previewing;
        if (m.item && g.detect_dirty) RunDetection();
        // The item moved or was trimmed, or the option changed: the plan follows (no new detection).
        else if (m.item && m.detected) BuildEvents();
        // 10-6: the item's pool at the same cadence, and at once on another item.
        if (count != g.sel_state_count || g.sel_at < 0.0 || now >= g.sel_at) {
            RefreshSelection();
            RefreshPool();
            g.sel_state_count = count;
            g.sel_at = now + kRereadSeconds;
        } else if (item_changed) {
            RefreshPool();
        }
        g.state_count = count;
    } catch (const std::exception& e) {
        LogError("Tagging view: %s", e.what());
    } catch (...) {
        LogError("Tagging view: unexpected error");
    }
}

const TaggingModel& GetTaggingModel()
{
    return g.model;
}

const ItemRules& TaggingShownRules()
{
    return g.previewing ? g.preview : g.model.rules;
}

void TaggingPreview(const ItemRules& rules, bool event_edit)
{
    if (!g.model.item) return;
    g.preview = rules;
    g.preview_event = event_edit;
    g.previewing = true;
    g.model.previewing = true;
    RunDetection();  // the strip and the markers follow at once
}

void TaggingCancelPreview()
{
    if (!g.previewing) return;
    g.previewing = false;
    g.model.previewing = false;
    RunDetection();
}

bool TaggingCommitPreview(const char* undo_desc)
{
    const bool event_edit = g.preview_event;
    if (!g.previewing) return false;
    const ItemRules shown = g.preview;
    auto write = [&](ItemRules& r) {
        r = shown;
        return true;
    };
    // An event's time preview is committed here only when a gesture is cut short (the view
    // changes); the inspector's own commit names it with its rule.
    return event_edit ? TaggingEditEvent("RAV: Set event time", write) : TaggingEdit(undo_desc, write);
}

bool TaggingEdit(const char* undo_desc, const std::function<bool(ItemRules&)>& edit, MediaItem* for_item)
{
    // Queued for another item (the playhead moved on): nothing is written.
    if (for_item && for_item != g.model.item) return false;
    g.last_error.clear();
    g.previewing = false;
    g.model.previewing = false;
    if (!g.model.item) {
        g.last_error = "No animation item under the playhead.";
        return false;
    }
    std::string err;
    bool ok = false;
    try {
        ok = ModifyItemRules(g.model.item, undo_desc, edit, &err);
    } catch (...) {
        ok = false;
        err = "The item's rules could not be saved.";
    }
    if (!ok) g.last_error = err.empty() ? "The item's rules could not be saved." : err;
    // Show what is saved now (read again at once, not on the next state change).
    ReadItem();
    RunDetection();
    return ok;
}

bool TaggingEditEvent(const char* undo_desc, const std::function<bool(ItemRules&)>& edit, MediaItem* for_item)
{
    // 10-4 fb-4: an event correction is a gesture like any other: the record and the item's
    // preview markers in one undo point (ModifyItemRules' hook); committed markers wait for Commit.
    return TaggingEdit(undo_desc, edit, for_item);
}

bool TaggingAnalyse()
{
    TaggingModel& m = g.model;
    if (!m.item) return false;
    if (m.missing_count > 0) {
        g.last_error = "Analyse needs every role mapped to a bone.";
        return false;
    }
    std::string why;
    const bool ok = TaggingEdit("RAV: Analyse thresholds", [&](ItemRules& r) {
        std::vector<Block> bound;
        if (!BindAndSample(r, &bound)) {
            why = "The item's bones could not be read.";
            return false;
        }
        AnalyseOptions ao = r.analyse;
        ao.smooth_ms = r.options.smooth_ms;
        const std::vector<Block> analysed = Analyse(bound, g.tracks, ao);
        CopyAnalysedValues(analysed, r.blocks);
        return true;
    });
    if (ok && !why.empty()) {
        g.last_error = why;
        return false;
    }
    return ok;
}

bool TaggingDetectAuto(const AutoSettings& settings)
{
    if (!g.model.item) return false;
    return TaggingEdit("RAV: Detect auto events", [settings](ItemRules& r) {
        bool                   changed = false;
        const std::vector<int> map = ApplyAutoSettings(r.blocks, settings, &changed);
        if (changed) RemapEventBlocks(r.events, map);
        // Story 10-8h: a block with no detected event (before the user's Suppress edits) gets the
        // lowest sensitivity that finds one, in this undo point.
        TaggingModel& m = g.model;
        if (HasActiveAutoBlocks(r.blocks) && EnsureRoleTracks()) {
            std::vector<AutoSensResult> res;
            if (ApplyAutoSensSearch(r.blocks, g.role_tracks, m.role_to_bone, &res, &g.auto_cache)) changed = true;
            for (size_t i = 0; i < res.size() && i < r.blocks.size(); ++i) {
                if (!res[i].searched) continue;
                const std::string key = AutoSearchKey(r.blocks[i]);
                m.auto_searched[key] = r.blocks[i].sens;
                if (res[i].none) m.auto_none[key] = r.blocks[i].sens;
                else m.auto_none.erase(key);
            }
        }
        return changed;
    });
}

std::string AutoSearchKey(const Block& b)
{
    return b.auto_type + ":" + std::string(1, b.auto_side);
}

bool AutoSearchedAt(const std::map<std::string, double>& in, const Block& b)
{
    const auto it = in.find(AutoSearchKey(b));
    return it != in.end() && it->second == b.sens;
}

bool TaggingLoadPreset(const std::string& preset_id)
{
    g.last_error.clear();
    g.previewing = false;
    g.model.previewing = false;
    if (!g.model.item) return false;
    std::string missing, err;
    const bool ok = SetUpPresetOnItem(g.model.item, preset_id, &missing, &err);
    if (!ok) g.last_error = err.empty() ? "The preset could not be loaded." : err;
    ReadItem();
    RunDetection();
    return ok;
}

bool TaggingSavePreset(const std::string& preset_id, const std::string& name, std::string* saved_name,
                       std::string* err)
{
    g.last_error.clear();
    g.previewing = false;
    g.model.previewing = false;
    if (!g.model.item) {
        if (err) *err = "No animation item under the playhead.";
        return false;
    }
    const bool ok = SaveItemAsPreset(g.model.item, preset_id, name, nullptr, saved_name, err);
    ReadItem();
    RunDetection();
    return ok;
}

bool TaggingUpdatePreset()
{
    g.last_error.clear();
    g.previewing = false;
    g.model.previewing = false;
    if (!g.model.item) return false;
    std::string err;
    const bool ok = UpdateItemFromPreset(g.model.item, &err);
    if (!ok) g.last_error = err.empty() ? "The rules could not be updated from the preset." : err;
    ReadItem();
    RunDetection();
    return ok;
}

bool TaggingKeepPreset()
{
    g.last_error.clear();
    if (!g.model.item) return false;
    std::string err;
    const bool ok = KeepItemCurrent(g.model.item, &err);
    if (!ok) g.last_error = err.empty() ? "The current rules could not be kept." : err;
    ReadItem();
    RunDetection();
    return ok;
}

void TaggingPresetFilesChanged()
{
    if (g.model.item) ReadPresetFields();
}

bool TaggingMakeUnique(MediaItem* for_item)
{
    // Queued for another item (the playhead moved on): nothing is written.
    if (for_item && for_item != g.model.item) return false;
    g.last_error.clear();
    if (!g.model.item) return false;
    std::string err;
    bool        done = false;
    const bool  ok = MakeItemUnique(g.model.item, &done, &err);
    if (!ok) g.last_error = err.empty() ? "The item could not be made unique." : err;
    ReadItem();
    RunDetection();
    RefreshSelection();
    RefreshPool();
    return ok && done;
}

ItemClipMap ItemClipMapOf(MediaItem* item, double clip_len)
{
    ItemClipMap map;
    map.clip_len = clip_len;
    if (!item) return map;
    MediaItem_Take* take = RavTakeOf(item);
    map.item_pos = GetMediaItemInfo_Value(item, "D_POSITION");
    map.item_len = GetMediaItemInfo_Value(item, "D_LENGTH");
    map.loop = GetMediaItemInfo_Value(item, "B_LOOPSRC") != 0.0;
    map.start_offs = take ? GetMediaItemTakeInfo_Value(take, "D_STARTOFFS") : 0.0;
    const double rate = take ? GetMediaItemTakeInfo_Value(take, "D_PLAYRATE") : 1.0;
    map.rate = (rate > 0.0 && std::isfinite(rate)) ? rate : 1.0;
    return map;
}

ItemDetection DetectItem(MediaItem* item)
{
    ItemDetection out;
    try {
        if (!item || !ValidatePtr2(nullptr, item, "MediaItem*")) return out;
        MediaItem_Take* take = RavTakeOf(item);
        if (!take) {
            out.status = ItemDetection::Status::NotRav;
            return out;
        }
        ItemRulesRead rd;
        if (!ReadItemRules(item, &rd) || !rd.present || !rd.valid || rd.rules.blocks.empty()) {
            out.status = ItemDetection::Status::NoRules;
            return out;
        }
        out.rules = rd.rules;
        const std::string path = AnimPathOf(take);
        // The file: the session's parse when it is the same one, else the cache's.
        std::shared_ptr<const CpuAsset> asset = (path == g.asset_path && g.asset) ? g.asset : AcquireCpuAsset(path);
        if (!asset || asset->animations.empty() || asset->skeleton.bones.empty()) {
            out.status = ItemDetection::Status::NoFile;
            return out;
        }
        out.map = ItemClipMapOf(item, asset->animations[0].duration);
        const SkeletonBones sk = SkeletonBonesOf(asset->skeleton);
        const std::vector<std::string>& names = sk.names;
        // 10-8b fb-1: a rule a missing role skips is bound as an off rule; the item is skipped as
        // a whole only when nothing else can run.
        const RoleMapping rm = ItemRoleMapping(names, out.rules.blocks);
        const BlockSkips  skips = SkippedBlocks(out.rules.blocks, rm.builtin, rm.custom, names, sk.parents, IsActiveAutoBlock);
        out.skipped = skips.skipped;
        std::vector<Block> bound = RunnableBlocks(out.rules.blocks, skips.skipped);
        if (skips.count > 0 && skips.nothing_runs) {
            // Each name once (two rules may miss the same role).
            std::vector<std::string> names_once;
            for (const std::string& nm : MissingBoneRefs(RunnableBlocks(out.rules.blocks), rm.builtin, rm.custom, names,
                                                         sk.parents))
                if (std::find(names_once.begin(), names_once.end(), nm) == names_once.end()) names_once.push_back(nm);
            for (const std::string& nm : names_once) out.missing += (out.missing.empty() ? "" : ", ") + nm;
            out.status = ItemDetection::Status::RolesMissing;
            return out;
        }
        if (!BindBoneRefs(bound, rm.builtin, rm.custom, names, sk.parents, &out.missing)) {
            out.status = ItemDetection::Status::RolesMissing;
            return out;
        }
        std::vector<int> bones;
        RemapToTracks(bound, &bones);
        const AssetFileStamp stamp = (path == g.asset_path) ? g.asset_stamp : ReadAssetFileStamp(path);
        SampleKey key{path, stamp, bones};
        std::vector<BoneTrack> tracks;
        if (g.have_tracks && key == g.sample_key) tracks = g.tracks;
        else if (!bones.empty()) tracks = SampleBoneTracks(*asset, bones, kDetectRateHz);
        if (!bones.empty() && tracks.size() != bones.size()) {
            out.status = ItemDetection::Status::Failed;
            return out;
        }
        out.events = Detect(bound, tracks, out.rules.options);
        // Story 10-8b: the auto blocks' events, on the item's role mapping (the session's role
        // tracks and analyses when they are this file's with the same bones).
        const std::vector<int>& role_to_bone = rm.builtin;
        if (HasActiveAutoBlocks(out.rules.blocks)) {
            const SampleKey rkey = RoleTrackKey(path, stamp, role_to_bone);
            if (g.have_role_tracks && rkey == g.role_key) {
                MergeEvents(out.events, DetectAutoEvents(out.rules.blocks, g.role_tracks, &g.auto_cache));
            } else {
                MergeEvents(out.events, DetectAutoEvents(out.rules.blocks, SampleRoleTracks(*asset, role_to_bone)));
            }
        }
        // 10-8b fb-1: the skipped rules and the auto blocks whose part has no bone keep their last
        // Commit's markers (Commit plans them from here, so it writes and records them again).
        KeepUnrunnableEvents(out.events, out.rules.events, out.rules.blocks, skips.skipped, role_to_bone);
        out.status = ItemDetection::Status::Ok;
    } catch (...) {
        out.status = ItemDetection::Status::Failed;
    }
    return out;
}

bool TaggingMeasureEvent(const ItemRules& rules, int block, double t, double* strength, double* speed)
{
    if (strength) *strength = 0.0;
    if (speed) *speed = 0.0;
    try {
        if (!g.model.item || block < 0 || block >= static_cast<int>(rules.blocks.size())) return false;
        std::vector<Block> bound;
        if (!BindAndSample(rules, &bound)) return false;
        return EventValuesAt(bound[static_cast<size_t>(block)], g.tracks, rules.options, t, strength, speed);
    } catch (...) {
        return false;
    }
}

void TaggingReread()
{
    if (!g.model.item) return;
    ReadItem();
    RunDetection();
    RefreshSelection();
    RefreshPool();
}

const std::string& TaggingLastError()
{
    return g.last_error;
}

void TaggingClearError()
{
    g.last_error.clear();
}

void TaggingSessionReset()
{
    g = Session{};
}

}  // namespace rav

#endif  // _WIN32
