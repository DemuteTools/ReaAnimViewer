// SPDX-License-Identifier: MIT
//
// Epic 13 (D20, story 13-2) -- the shared sound model: Switch Groups, sound libraries (a tree
// of Switch / Random / Blend / Sound nodes), the per-animation-item sound state (values,
// draws, placements) and the tag on each item RAV places. Pure, header only, no REAPER.
//
// FROZEN after 13-2 (architecture.md D20): a wave story never edits this file; a needed change
// goes to the orchestrator. Text grammars: sound_record.h. Ids are random 16-hex strings
// (NewSoundUid, rule_record.h), never indices.
//
// 13-15a: append-only extension (assignment layers), orchestrator decision: SoundAssign,
// SoundCharacter, SoundStore::project / characters, SoundLibrary::kit, RuleSounds::none.
//
// Every `kept` member holds text this version did not understand, written back as read:
// unknown fields ("key=value" tokens) of the object's own line, and `kept_lines` the unknown
// lines read right after it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rav {

enum class SoundNodeType { Sound, Random, Blend, Switch };

// How a file is found again (13-5): the name ignoring case, the size, the length (s, +-1.5 ms);
// the paths are hints; mtime is a hint only, never a match key.
struct AssetIdentity {
    std::string name;            // file name with extension
    int64_t     size = -1;       // bytes, -1 = unknown
    double      length = -1.0;   // seconds, -1 = unknown
    std::string last_path;       // the absolute path last seen
    std::string rel_path;        // relative to the project folder, "" = none
    int64_t     mtime = 0;       // hint only
};

// How a Sound's variations (frames) were detected (13-3).
enum class DetectMode { Whole, Takes, TakeMarkers, Silence };

struct SilenceParams {
    double threshold_db = -40.0;
    double min_gap_ms = 50.0;
    double preroll_ms = 5.0;
    double tail_ms = 50.0;
};

// One variation of a Sound: a stretch of its source (source seconds).
struct SoundFrame {
    double                   start = 0.0;
    double                   length = 0.0;
    bool                     on = true;    // part of the draw
    double                   db = 0.0;     // per-variation gain
    bool                     hand = false; // edited by hand (kept across a re-detect)
    std::vector<std::string> kept;
    // Fades (Antho 2026-10-12; after `kept` so brace-initialised frames keep their meaning): seconds,
    // < 0 = none set here (the Sound's default fade applies). Shape = REAPER's C_FADEINSHAPE /
    // C_FADEOUTSHAPE index (0 = linear).
    double                   fade_in = -1.0;
    double                   fade_out = -1.0;
    int                      fade_in_shape = 0;
    int                      fade_out_shape = 0;
};

// The Sound-level default fades (s): every variation with no fade of its own gets them.
constexpr double kDefaultFadeIn = 0.001;
constexpr double kDefaultFadeOut = 0.005;

enum class SoundSourceKind { Item, File };

// Where a Sound comes from: an item of the project (its GUID) or a file (its identity).
struct SoundSource {
    SoundSourceKind kind = SoundSourceKind::Item;
    std::string     item_guid;
    AssetIdentity   asset;
};

// A Sound leaf's data. `frames` = the detected (pre-edit) variations, stored: Populate never
// re-detects (D6).
struct SoundData {
    SoundSource              source;
    DetectMode               mode = DetectMode::Whole;
    SilenceParams            silence;
    std::vector<SoundFrame>  frames;
    bool                     edited = false;        // frames edited after detection
    bool                     linked_copies = true;  // copies of the pile share the takes
    std::vector<std::string> kept;
    double                   fade_in = kDefaultFadeIn;    // default fades of its variations (s)
    double                   fade_out = kDefaultFadeOut;
};

// A Switch's value -> child.
struct SwitchCase {
    std::string              value_uid;
    std::string              child_uid;
    std::vector<std::string> kept;
};

struct SoundNode {
    std::string              uid;
    std::string              parent;  // "" = the library's root
    SoundNodeType            type = SoundNodeType::Random;
    std::string              name;
    double                   volume_db = 0.0;   // on the node (multiplies down the tree)
    double                   offset_ms = 0.0;   // Blend: this child's offset in its Blend parent
    int                      avoid_repeat = 2;  // Random: avoid repeating the last N
    // Switch only.
    std::string              group_uid;
    std::vector<SwitchCase>  cases;
    std::string              default_value;   // value uid ("" = the group's first)
    std::string              preview_value;   // value uid ("" = the default)
    std::vector<std::string> unassigned;      // child uids on no value (the orange list)
    // User Library sounds (13-11): the destination folder ("Lands in").
    std::string              lands_in;
    // Sound only.
    SoundData                sound;
    std::vector<std::string> kept;
    std::vector<std::string> kept_lines;
};

struct SwitchValue {
    std::string              uid;
    std::string              name;
    std::vector<std::string> kept;
};

struct SwitchGroup {
    std::string              uid;
    std::string              name;
    std::vector<SwitchValue> values;
    std::vector<std::string> kept;
    std::vector<std::string> kept_lines;
};

struct SoundLibrary {
    std::string              uid;
    std::string              name;
    uint32_t                 color = 0;   // 0 = none, else 0x1000000 | 0xRRGGBB (Block::color form)
    std::string              root;        // the root node's uid
    bool                     user = false;  // a User Library (13-11)
    std::string              ravlib_path;   // its .ravlib file (13-11)
    std::vector<SoundNode>   nodes;         // children in line order
    std::vector<std::string> kept;
    std::vector<std::string> kept_lines;
    // 13-15a: the body parts this library suggests itself for (a kit, sound_assign.h part ids); it
    // travels in the .ravlib and is never read by resolution. `kit_offered`: the kit was offered
    // to the project already.
    std::vector<std::string> kit;
    bool                     kit_offered = false;
    std::vector<std::string> kit_kept;  // unknown fields of its `kit` line
};

// A Switch Group's value at one level (item, rule, event, auto marker type).
struct ValueSet {
    std::string              group_uid;
    std::string              value_uid;
    std::vector<std::string> kept;
};

// The sound an auto marker type gets (Antho 2026-10-12, 13-8): the library and rule-level Switch
// values the Tagging view's Auto detection section sets per marker type, before or after Detect.
// Detect gives it to the rule of each auto block of that key with no library yet; an edit in the
// section or in an auto block's Sound card writes both (sound_auto_assign.h).
struct AutoSoundAssign {
    std::string              key;      // AutoSoundKey (sound_auto_assign.h): "step:L", "slide_heel:R"...
    std::string              library;  // library uid ("" = none)
    std::vector<ValueSet>    values;   // rule-level Switch values
    std::vector<std::string> kept;
    std::vector<std::string> kept_lines;
};

// ---- 13-15a: assignment layers (Kit > Project > Character > Item > Event) -----------------------

// inherit = this level says nothing; None = place nothing, on purpose; Library = that library.
enum class LibState { Inherit, None, Library };

// One target at Project or Character level: a body part ("feet") or a part and type ("feet.step";
// the type is an AutoType word, or "rules" for the bone rules of that part).
struct SoundAssign {
    std::string              key;
    LibState                 state = LibState::Inherit;
    std::string              library;       // state Library: its uid
    bool                     kit = false;   // adopted from a library's kit
    std::vector<ValueSet>    values;        // Switch values at this target
    std::vector<std::string> kept;
    std::vector<std::string> kept_lines;
};

// A rig the project knows (recognised by its skeleton signature), with its own assignments.
struct SoundCharacter {
    std::string              uid;
    std::string              name;
    std::string              skeleton;    // SkeletonKey (16 hex)
    std::string              model_file;  // "" = the whole rig; else a split's file name
    std::vector<std::string> files;       // model file names seen (a hint)
    std::vector<SoundAssign> assigns;
    std::vector<std::string> kept;
    std::vector<std::string> kept_lines;
};

// The project's store (the .rpp's <RAVSOUNDS chunk).
struct SoundStore {
    int                       version = 1;
    std::vector<SwitchGroup>  groups;
    std::vector<SoundLibrary> libraries;
    std::vector<std::string>  kept;  // unknown fields of the header line
    std::vector<std::string>  kept_lines;  // unknown lines right after the header line
    // Antho 2026-10-12 (13-8): each auto marker type's sound (written right after the header).
    std::vector<AutoSoundAssign> auto_sounds;  // 13-15a: read for migration only, never written
    // 13-15a: the project's and the characters' assignments.
    std::vector<SoundAssign>     project;
    std::vector<SoundCharacter>  characters;
};

// ---- Per-animation-item state (the take's P_EXT:RAV_SOUNDS) -------------------------------------

// A draw: which variation (pick) a Random / Sound node gave this event.
struct SoundDraw {
    std::string              node;
    int                      pick = -1;
    bool                     hand = false;  // picked by hand: kept on Reshuffle
    std::vector<std::string> kept;
};

enum class Ownership { Populated, Attached, Spread, Deleted };

struct SoundPlacement {
    std::string              item_guid;
    std::string              node;
    Ownership                own = Ownership::Populated;
    double                   offset_s = 0.0;  // the user's nudge, kept while following
    int                      variation = -1;
    std::string              track_guid;
    std::vector<std::string> kept;
};

// One event = (rule id, clip time), matched within +-30 ms.
struct EventSounds {
    std::string                 rid;
    double                      t = 0.0;
    std::vector<ValueSet>       overrides;  // event-level Switch values
    std::vector<SoundDraw>      draws;
    std::vector<SoundPlacement> placed;
    std::vector<std::string>    kept;
    std::vector<std::string>    kept_lines;
};

struct RuleSounds {
    std::string              rid;
    std::string              library;  // library uid ("" and !none = inherit)
    std::vector<ValueSet>    values;   // rule-level Switch values
    std::vector<std::string> kept;
    std::vector<std::string> kept_lines;
    bool                     none = false;  // 13-15a: "None (place nothing)"
};

struct ItemSounds {
    int                       version = 1;
    uint64_t                  seed = 0;
    std::vector<ValueSet>     item_values;  // item-level Switch values
    std::vector<RuleSounds>   rules;
    std::vector<EventSounds>  events;
    std::vector<std::string>  kept;
    std::vector<std::string>  kept_lines;
};

// The tag on each item RAV placed (item P_EXT:RAV_SND). Never on a folder item.
struct PlacedTag {
    std::string owner;  // the animation item's GUID
    std::string rid;
    double      t = 0.0;
    std::string node;
    Ownership   own = Ownership::Populated;
    // 13-8 (version 2): how the item was placed. `ao` = its offset from the pile's anchor (a folder
    // pile member: where it was put minus the anchor, so a member clamped at 0 still inverts; else 0),
    // `sy` = the snap the anchor was placed by. Without them (has_anchor false) the item's own
    // position and snap are read.
    bool        has_anchor = false;
    double      ao = 0.0;
    double      sy = 0.0;
    int         version = 2;      // a higher version is read best-effort and kept on write
    std::vector<std::string> kept;  // unknown fields, written back after the known ones
};

// ---- Shared working types --------------------------------------------------------------------

// A slice of a source (take markers / .rpp markers; 13-3).
struct Slice {
    double      start = 0.0;
    double      length = 0.0;
    std::string name;
};

// Peaks of a source (13-7b reads them). `max_abs` is the max |sample| over every channel: what
// detection reads (SplitOnSilence, FramePeakDb, NormalizeFrames). 13-14: the waveform's lanes,
// one for a mono source, two (L, R) for a source of 2 channels or more (channel c goes to lane
// c % 2). A mono source keeps `lanes` empty: its one lane is `max_abs`, so its memory is unchanged;
// a stereo one adds two series the size of `max_abs`. Read them with PeakChannels / PeakLane.
struct PeakData {
    double             rate = 0.0;  // peaks per second
    std::vector<float> max_abs;
    int                channels = 1;  // lanes: 1 (mono) or 2 (L, R)
    std::vector<float> lanes[2];      // channels == 2: max |sample| of each lane; else empty
};

// ---- Peak lanes (13-14): pure helpers shared by the peak readers and the waveform drawers ----

// The lanes to draw: 2 when the per-lane series are there, else 1 (a mono source, or peaks read
// without lanes).
inline int PeakChannels(const PeakData& p)
{
    return p.channels == 2 && p.lanes[0].size() == p.max_abs.size() && p.lanes[1].size() == p.max_abs.size() ? 2 : 1;
}

// The series of lane `ch` (max |sample| per bin; the lane spans -v..+v). Mono, or a lane out of
// range: `max_abs`.
inline const std::vector<float>& PeakLane(const PeakData& p, int ch)
{
    return PeakChannels(p) == 2 && (ch == 0 || ch == 1) ? p.lanes[ch] : p.max_abs;
}

// The max of lane `ch` over bins [i0, i1) (clamped; 0 when empty): the lane is drawn from -max to
// +max.
inline float PeakLaneMax(const PeakData& p, int ch, size_t i0, size_t i1)
{
    const std::vector<float>& v = PeakLane(p, ch);
    float                     m = 0.0f;
    for (size_t i = i0; i < i1 && i < v.size(); ++i) m = v[i] > m ? v[i] : m;
    return m;
}

// Sizes `p` for `bins` bins of a source of `source_channels` channels (all bins 0): the lanes only
// when it has 2 channels or more. `rate` is left to the caller.
inline void PeakInit(PeakData* p, int source_channels, size_t bins)
{
    p->max_abs.assign(bins, 0.0f);
    p->channels = source_channels >= 2 ? 2 : 1;
    for (int l = 0; l < 2; ++l) {
        if (p->channels == 2) p->lanes[l].assign(bins, 0.0f);
        else std::vector<float>().swap(p->lanes[l]);
    }
}

// Folds one sample frame (`nch` interleaved values; |v| is taken) into bin `bin`: `max_abs` gets the
// max over all channels, lane c % 2 the max of its channels. A bin out of range is ignored.
template <typename Sample>
inline void PeakAddFrame(PeakData* p, size_t bin, const Sample* frame, int nch)
{
    if (bin >= p->max_abs.size()) return;
    float m = 0.0f, lane[2] = {0.0f, 0.0f};
    for (int c = 0; c < nch; ++c) {
        const Sample s = frame[c];
        const float  a = static_cast<float>(s < 0 ? -s : s);
        m = m < a ? a : m;  // as std::max(m, a): a NaN sample is skipped
        lane[c & 1] = lane[c & 1] < a ? a : lane[c & 1];
    }
    p->max_abs[bin] = p->max_abs[bin] < m ? m : p->max_abs[bin];
    if (p->channels == 2 && bin < p->lanes[0].size() && bin < p->lanes[1].size())
        for (int l = 0; l < 2; ++l) p->lanes[l][bin] = p->lanes[l][bin] < lane[l] ? lane[l] : p->lanes[l][bin];
}

enum class ReportKind { Missing, Replaced, Detached, Blocked, Info };  // Info (13-11): not counted

struct ReportLine {
    ReportKind  kind = ReportKind::Missing;
    std::string text;
    std::string node;
    std::string item_guid;
    double      t = 0.0;
};

struct SoundReport {
    std::vector<ReportLine> lines;
};

}  // namespace rav
