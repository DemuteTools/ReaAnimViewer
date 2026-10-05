// SPDX-License-Identifier: MIT
//
// See tagging_session.h.

#include "tagging_session.h"

#ifdef _WIN32

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

// The blocks as detection and the binding see them: an off rule reads no bone (its
// conditions' and strength's bone lists are emptied), so a role it alone needs is never
// missing. Block indices stay, so the trace lines up with the record.
std::vector<Block> EnabledOnly(const std::vector<Block>& blocks)
{
    std::vector<Block> out = blocks;
    for (Block& b : out) {
        if (b.enabled) continue;
        for (Condition& c : b.conditions) {
            c.signal.bones.clear();
            c.signal.ref_bones.clear();
        }
        b.strength_signal.bones.clear();
        b.strength_signal.ref_bones.clear();
    }
    return out;
}

// How many bone references the blocks read that have no bone on this skeleton, and their names.
// A joint angle whose joint has no parent or child counts too ("<name> (not a joint)").
int CountMissing(const std::vector<Block>& blocks, const std::vector<int>& role_to_bone,
                 const std::vector<std::string>& names, const std::vector<int>& parents, std::string* missing)
{
    const std::vector<std::string> list = MissingBoneRefs(blocks, role_to_bone, names, parents);
    std::string all;
    for (const std::string& nm : list) all += (all.empty() ? "" : ", ") + nm;
    if (missing) *missing = all;
    return static_cast<int>(list.size());
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
    m.has_rules = rd.present && rd.valid;
    m.rules = m.has_rules ? rd.rules : ItemRules{};
    m.has_preset = m.has_rules && m.rules.has_preset;
    m.preset_name = m.has_preset ? m.rules.preset_copy.name : "";
    m.preset_factory = m.has_preset && IsFactoryPresetId(m.rules.preset_copy.id);
    m.preset_state = m.has_preset ? GetPresetState(RulesResourceRoot(), m.rules) : PresetState::Unknown;
    m.role_to_bone = m.bone_names.empty() ? std::vector<int>{} : GetRoleMapping(RulesResourceRoot(), m.bone_names);
    m.missing.clear();
    m.missing_count = m.file_loaded ? CountMissing(EnabledOnly(m.rules.blocks), m.role_to_bone, m.bone_names, m.bone_parents, &m.missing) : 0;
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
    if (!m.detected) return;
    const ItemRules& shown = TaggingShownRules();
    m.events = BuildEventList(m.trace.events, shown.events, shown.blocks.size());
    m.planned = PlanMarkers(m.events, shown.blocks, m.map);
    m.markers_up_to_date = MarkersUpToDate(m.rules, m.planned, GetTaggingMarkerMode());
}

// Binds `rules` on the skeleton and turns them into track indices; samples the bones when
// the bone set (or the file) changed. False when they do not bind or cannot be sampled.
bool BindAndSample(const ItemRules& rules, std::vector<Block>* bound_out)
{
    TaggingModel& m = g.model;
    if (!m.file_loaded || !g.asset) return false;
    std::vector<Block> bound = EnabledOnly(rules.blocks);
    std::string why;
    if (!BindBoneRefs(bound, m.role_to_bone, m.bone_names, m.bone_parents, &why)) return false;
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

void RunDetection()
{
    TaggingModel& m = g.model;
    g.detect_dirty = false;
    m.detected = false;
    m.trace = DetectionTrace{};
    m.events.clear();
    m.planned.clear();
    m.markers_up_to_date = false;
    if (m.missing_count > 0) return;  // no detection run (the strip says so)
    const ItemRules& shown = TaggingShownRules();
    std::vector<Block> bound;
    if (!BindAndSample(shown, &bound)) return;
    m.trace = DetectTrace(bound, g.tracks, shown.options);
    m.detected = true;
    BuildEvents();
}

// The footer's counts: the selected items, those without rules, those whose roles miss.
void RefreshSelection()
{
    TaggingModel& m = g.model;
    m.sel_count = m.sel_without_rules = m.sel_roles_skipped = 0;
    const int n = CountSelectedMediaItems(nullptr);
    m.sel_count = n;
    for (int i = 0; i < n; ++i) {
        MediaItem* it = GetSelectedMediaItem(nullptr, i);
        ItemRulesRead rd;
        if (!it || !ReadItemRules(it, &rd) || !rd.present || !rd.valid || rd.rules.blocks.empty()) {
            ++m.sel_without_rules;
            continue;
        }
        const std::string path = AnimPathOf(RavTakeOf(it));
        const SkeletonBones& sk = BoneNamesOf(path);
        const std::vector<std::string>& names = sk.names;
        std::vector<Block> probe = EnabledOnly(rd.rules.blocks);
        if (names.empty() ||
            !BindBoneRefs(probe, GetRoleMapping(RulesResourceRoot(), names), names, sk.parents, nullptr))
            ++m.sel_roles_skipped;
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
            if (!item) {
                // The file goes too: the next item (even of the same file) loads it again.
                g.asset_path.clear();
                g.asset.reset();
                g.asset_stamp = AssetFileStamp{};
                g.have_tracks = false;
                g.stamp_checked_at = -1.0;
                const int sc = m.sel_count, snr = m.sel_without_rules, srs = m.sel_roles_skipped;
                m = TaggingModel{};
                m.sel_count = sc;
                m.sel_without_rules = snr;
                m.sel_roles_skipped = srs;
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
                g.reread_at = now + kRereadSeconds;
            }
        }
        m.previewing = g.previewing;
        if (m.item && g.detect_dirty) RunDetection();
        // The item moved or was trimmed, or the option changed: the plan follows (no new detection).
        else if (m.item && m.detected) BuildEvents();
        if (count != g.sel_state_count || g.sel_at < 0.0 || now >= g.sel_at) {
            RefreshSelection();
            g.sel_state_count = count;
            g.sel_at = now + kRereadSeconds;
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
    if (for_item && for_item != g.model.item) return false;
    MediaItem* item = g.model.item;
    ItemRulesRead cur;
    // Never applied, markers not up to date (or no item, or a record that does not read):
    // exactly as before.
    // Auto-apply only when the markers were up to date BEFORE the edit (from the SAVED record,
    // not the preview): a rule edit or option change pending since the last Apply never
    // reaches the timeline through an event correction.
    auto up_to_date_before = [&]() {
        const ItemDetection det = DetectItem(item);
        if (det.status != ItemDetection::Status::Ok) return false;
        const std::vector<ShownEvent>    list = BuildEventList(det.events, det.rules.events, det.rules.blocks.size());
        const std::vector<PlannedMarker> plan = PlanMarkers(list, det.rules.blocks, det.map);
        return MarkersUpToDate(det.rules, plan, GetTaggingMarkerMode());
    };
    if (!item || !ReadItemRules(item, &cur) || !cur.present || !cur.valid || !cur.rules.has_applied ||
        !up_to_date_before()) {
        const bool ok = TaggingEdit(undo_desc, edit, for_item);
        if (ok) ClearLastApplyResult();  // the footer's Apply line is stale now
        return ok;
    }

    g.last_error.clear();
    g.previewing = false;
    g.model.previewing = false;
    std::string err;
    bool ok = false;
    bool block_open = false;  // an undo block is open: it is always closed, even on a throw
    const char* desc = undo_desc ? undo_desc : "RAV: Edit auto-tagging event";
    try {
        // The edit runs once, on the record as saved; nothing is written (and no undo point
        // made) when it cancels or changes nothing.
        ItemRules next = cur.rules;
        if (!edit || !edit(next)) {
            ok = true;
        } else if (const std::string text = SerializeItemRules(next);
                   text == cur.raw || text == SerializeItemRules(cur.rules)) {
            ok = true;
        } else {
            Undo_BeginBlock2(nullptr);
            block_open = true;
            ok = ModifyItemRulesNoUndo(
                item,
                [&](ItemRules& r) {
                    r = next;
                    return true;
                },
                &err);
            ApplyResult ar;
            if (ok) ar = ApplyItemMarkersNoUndo(item);
            block_open = false;
            Undo_EndBlock2(nullptr, desc, UNDO_STATE_ALL);
            UpdateArrange();
            if (ok) {
                // The footer: "markers up to date" comes from the record; only a failure is told.
                if (!ar.error.empty()) {
                    ApplyResult shown;
                    shown.error = "Event saved, but its markers could not be written: " + ar.error + ".";
                    SetLastApplyResult(shown);
                } else if (ar.items == 0) {
                    ApplyResult shown;
                    shown.error = ar.roles_skipped > 0
                                      ? "Event saved, but its markers could not be written: a role has no bone."
                                      : "Event saved, but its markers could not be written.";
                    SetLastApplyResult(shown);
                } else {
                    ClearLastApplyResult();
                }
            }
        }
    } catch (...) {
        if (block_open) {
            Undo_EndBlock2(nullptr, desc, UNDO_STATE_ALL);
            UpdateArrange();
        }
        ok = false;
        err = "The item's rules could not be saved.";
    }
    if (!ok) g.last_error = err.empty() ? "The item's rules could not be saved." : err;
    ReadItem();
    RunDetection();
    return ok;
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
        std::vector<Block> bound = EnabledOnly(out.rules.blocks);
        if (!BindBoneRefs(bound, GetRoleMapping(RulesResourceRoot(), names), names, sk.parents, &out.missing)) {
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
