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
// Every `kept` member holds text this version did not understand, written back as read:
// unknown fields ("key=value" tokens) of the object's own line, and `kept_lines` the unknown
// lines read right after it.

#pragma once

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
};

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
};

// The project's store (the .rpp's <RAVSOUNDS chunk).
struct SoundStore {
    int                       version = 1;
    std::vector<SwitchGroup>  groups;
    std::vector<SoundLibrary> libraries;
    std::vector<std::string>  kept;  // unknown fields of the header line
    std::vector<std::string>  kept_lines;  // unknown lines right after the header line
};

// ---- Per-animation-item state (the take's P_EXT:RAV_SOUNDS) -------------------------------------

struct ValueSet {
    std::string              group_uid;
    std::string              value_uid;
    std::vector<std::string> kept;
};

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
    std::string              library;  // library uid ("" = none)
    std::vector<ValueSet>    values;   // rule-level Switch values
    std::vector<std::string> kept;
    std::vector<std::string> kept_lines;
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

// Mono peaks of a source (13-7b reads them).
struct PeakData {
    double             rate = 0.0;  // peaks per second
    std::vector<float> max_abs;
};

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
