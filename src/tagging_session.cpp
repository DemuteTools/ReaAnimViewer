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

// The animation file under a take (through a section / reversed wrapper).
std::string AnimPathOfTake(MediaItem_Take* take)
{
    PCM_source* src = take ? GetMediaItemTake_Source(take) : nullptr;
    if (!src || !IsRavAnimSource(src)) return "";
    for (int depth = 0; src && depth < 4; ++depth) {
        const char* type = src->GetType();
        if (type && std::strcmp(type, "RAV_ANIM") == 0) break;
        src = src->GetSource();
    }
    const char* fn = src ? src->GetFileName() : nullptr;
    return fn ? fn : "";
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
    bool      detect_dirty = true;
    std::string last_error;
    // The selection's footer.
    double sel_at = -1.0;
    int    sel_state_count = -1;
    // Bone names per file, for the selection's role check (parsed once per session).
    std::map<std::string, std::vector<std::string>> names_by_path;
};

Session g;

const std::vector<std::string>& BoneNamesOf(const std::string& path)
{
    auto it = g.names_by_path.find(path);
    if (it != g.names_by_path.end()) return it->second;
    std::vector<std::string> names;
    if (path == g.asset_path && g.asset) {
        for (const SceneBone& b : g.asset->skeleton.bones) names.push_back(b.name);
    } else if (const std::shared_ptr<const CpuAsset> a = AcquireCpuAsset(path)) {
        for (const SceneBone& b : a->skeleton.bones) names.push_back(b.name);
    }
    if (g.names_by_path.size() > 64) g.names_by_path.erase(g.names_by_path.begin());  // one out, not all
    return g.names_by_path[path] = std::move(names);
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
int CountMissing(const std::vector<Block>& blocks, const std::vector<int>& role_to_bone,
                 const std::vector<std::string>& names, std::string* missing)
{
    int n = 0;
    std::string all;
    for (int id : BoneRefsUsed(blocks)) {
        Block one;
        Condition c;
        c.signal.bones = {id};
        one.conditions.push_back(c);
        std::vector<Block> probe = {one};
        std::string why;
        if (!BindBoneRefs(probe, role_to_bone, names, &why)) {
            ++n;
            all += (all.empty() ? "" : ", ") + why;
        }
    }
    if (missing) *missing = all;
    return n;
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
    m.missing_count = m.file_loaded ? CountMissing(EnabledOnly(m.rules.blocks), m.role_to_bone, m.bone_names, &m.missing) : 0;
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
    if (g.asset)
        for (const SceneBone& b : g.asset->skeleton.bones) m.bone_names.push_back(b.name);
    m.file_loaded = g.asset && !g.asset->animations.empty() && !m.bone_names.empty();
    m.map.clip_len = (g.asset && !g.asset->animations.empty()) ? g.asset->animations[0].duration : 0.0;
    g.read_done = false;  // the binding depends on the bones: read again
    g.reread_at = -1.0;
}

void ReadItemTiming()
{
    TaggingModel& m = g.model;
    MediaItem_Take* take = RavTakeOf(m.item);
    m.map.item_pos = GetMediaItemInfo_Value(m.item, "D_POSITION");
    m.map.item_len = GetMediaItemInfo_Value(m.item, "D_LENGTH");
    m.map.loop = GetMediaItemInfo_Value(m.item, "B_LOOPSRC") != 0.0;
    m.map.start_offs = take ? GetMediaItemTakeInfo_Value(take, "D_STARTOFFS") : 0.0;
    const double rate = take ? GetMediaItemTakeInfo_Value(take, "D_PLAYRATE") : 1.0;
    m.map.rate = (rate > 0.0 && std::isfinite(rate)) ? rate : 1.0;
    const char* nm = take ? GetTakeName(take) : nullptr;
    m.item_name = (nm && nm[0]) ? nm : "(unnamed item)";
}

// Binds `rules` on the skeleton and turns them into track indices; samples the bones when
// the bone set (or the file) changed. False when they do not bind or cannot be sampled.
bool BindAndSample(const ItemRules& rules, std::vector<Block>* bound_out)
{
    TaggingModel& m = g.model;
    if (!m.file_loaded || !g.asset) return false;
    std::vector<Block> bound = EnabledOnly(rules.blocks);
    std::string why;
    if (!BindBoneRefs(bound, m.role_to_bone, m.bone_names, &why)) return false;
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
    if (m.missing_count > 0) return;  // no detection run (the strip says so)
    const ItemRules& shown = TaggingShownRules();
    std::vector<Block> bound;
    if (!BindAndSample(shown, &bound)) return;
    m.trace = DetectTrace(bound, g.tracks, shown.options);
    m.detected = true;
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
        const std::string path = AnimPathOfTake(RavTakeOf(it));
        const std::vector<std::string>& names = BoneNamesOf(path);
        std::vector<Block> probe = EnabledOnly(rd.rules.blocks);
        if (names.empty() || !BindBoneRefs(probe, GetRoleMapping(RulesResourceRoot(), names), names, nullptr))
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

void TaggingPreview(const ItemRules& rules)
{
    if (!g.model.item) return;
    g.preview = rules;
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
    if (!g.previewing) return false;
    const ItemRules shown = g.preview;
    return TaggingEdit(undo_desc, [&](ItemRules& r) {
        r = shown;
        return true;
    });
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
