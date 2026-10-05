// SPDX-License-Identifier: MIT
//
// The per-item rules record and the preset file (Epic 10, story 10-2): one text grammar,
// versioned, that keeps whatever it does not understand.
//
// An item's record lives in its active take's P_EXT:RAV_RULES (item_rules.h):
//
//   RAVRULES 1
//   options sensitivity=0 edge_ms=0 smooth_ms=8
//   analyse floor_pct=2 pos_frac=0.4 speed_pct=30 margin_ratio=0.5 onset_frac=0.3 per_bone_floor=1
//   preset id=factory/footsteps version=1 kept=0 name=Footsteps
//   copy
//   options ...          (the preset's options and analyse settings, as copied)
//   analyse ...
//   block color=#5F9EDD hold_ms=0 cooldown_ms=250 offset_ms=0 land=peak:1:max marker=Footstep L
//   cond q=point bones=role:left_heel,role:left_toe comb=lowest ref=floor meas=position axis=vertical dir=below thr=0.05 margin=0.025 fixed=0
//   strength q=point bones=role:left_heel,role:left_toe comb=lowest ref=floor meas=speed axis=vertical signed=1 sign=-1 window_ms=100
//   end
//   block ...            (the item's own blocks, the ones detection runs)
//   cond ...
//   event ...            (reserved for 10-4, kept as written)
//
// A preset file (.ravpreset) is the same grammar under "RAVPRESET 1": a `preset` line
// (no `kept`), optional `options` / `analyse`, then its blocks (no copy/end/event).
//
// Rules of the grammar:
//   - one record per line: a kind word, then `key=value` fields separated by one space.
//     A free-text field (`marker`, `name`) comes last and runs to the end of the line.
//   - roles and bones are stable string keys, never indices: `role:left_heel`,
//     `bone:<raw name>` (space, ',', '=', '%' and control bytes percent-encoded).
//   - numbers are written and read in the "C" locale (std::to_chars / from_chars, the
//     shortest form that reads back exactly).
//   - an absent field reads as its neutral default (the struct's default), so a v1 record
//     behaves the same after a later option is added.
//   - an unknown record kind, an unknown field on a known line, or a known field whose
//     value does not read, is kept and written back byte-identical, in place. A higher
//     version is read best-effort and keeps its version number on write.
//   - what is kept belongs to an object (KeptText, bone_events.h), never to a position:
//     a field to the line it was read on (block, cond, strength, options, analyse,
//     preset, copy, end); an unknown line to the object whose line it followed (the
//     header area before any, the tail after an event). It moves with its object when
//     blocks or conditions are inserted, deleted or reordered, and goes with it.
//   - a known field whose value did not read is written back raw only while the model
//     still holds the value it had after the read: an edit wins.
//   - `color` is "none" or "#RRGGBB" (Block::color = 0x1000000 | 0xRRGGBB).
//   - `land` is "cross" or "peak:<condition>:max|min".
//   - `on=0` on a `block` line = the rule is switched off (absent = on, story 10-3).
//
// In a parsed record, SignalSpec bones / ref_bones hold bone-reference ids (see
// BoneRefId), not track indices: BindBoneRefs turns them into skeleton bone indices.
//
// Pure C++17: no REAPER, no GL, no _WIN32. Host-tested (tests/rule_record_test.cpp).

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "bone_events.h"
#include "bone_roles.h"

namespace rav {

// ---- Bone references -------------------------------------------------------------------

// Ids 0 .. Role::Count-1 are the roles. Any other key (an unknown role "role:left_hand",
// a bone "bone:mixamorig:LeftFoot") gets an id >= kBoneRefExtra, interned for the life of
// the process (thread-safe), so equal keys always have equal ids.
constexpr int kBoneRefExtra = 1000;

// The id of a decoded key ("role:left_heel", "bone:<raw name>"). -1 for an empty key.
int BoneRefId(const std::string& key);
// = BoneRefId("bone:" + raw_name).
int BoneRefForBone(const std::string& raw_name);
// The decoded key of an id ("" when the id is neither a role nor interned).
std::string BoneRefKey(int id);
// Human name, for a missing list: "left toe" (RoleName), "left hand" (an unknown role key,
// '_' read as ' '), or the bone's raw name.
std::string BoneRefName(int id);
// Short label, for signal names: "L heel", "hips", an unknown role as BoneRefName, the
// bone's raw name.
std::string BoneRefLabel(int id);

// Every id the blocks read (conditions, references, strength), each once, in first-use order.
std::vector<int> BoneRefsUsed(const std::vector<Block>& blocks);

// Binds the ids to skeleton bone indices: a role through role_to_bone (indexed by Role,
// -1 = unmapped), a bone key by its raw name in bone_names (exact, else the same
// NormalizeBoneName). False when one cannot be bound; `missing` then lists their names,
// each once, ", "-separated (an unknown role key is reported, never dropped).
bool BindBoneRefs(std::vector<Block>& blocks, const std::vector<int>& role_to_bone,
                  const std::vector<std::string>& bone_names, std::string* missing);

// ---- The record ------------------------------------------------------------------------

struct PresetCopy {
    std::string        id;                // "factory/footsteps", "user/my-steps"
    int                version = 0;
    int                kept_version = 0;  // Keep current: the preset version the user dismissed
    std::string        name;
    DetectOptions      options;           // the preset's options and analyse settings as copied
    AnalyseOptions     analyse;
    std::vector<Block> blocks;            // the preset's blocks as copied (roles)
    KeptText           kept;              // the `preset` line
    KeptText           copy_kept;         // the `copy` line
    KeptText           end_kept;          // the `end` line
};

// An unknown line read after an `event` line: written back after the same number of
// events (events are opaque until 10-4), after the last one when there are fewer now.
struct RecordTailLine {
    size_t      after_events = 0;
    std::string text;
};

struct ItemRules {
    int                         format_version = 1;  // the version read (kept on write), at least 1
    std::string                 header_rest;         // the header line after the version, as written
    DetectOptions               options;
    AnalyseOptions              analyse;
    bool                        has_preset = false;
    PresetCopy                  preset_copy;
    std::vector<Block>          blocks;  // the item's rules (roles / bone keys)
    std::vector<std::string>    events;  // raw `event` lines (10-4), kept as written
    KeptText                    head;    // unknown lines right after the header line
    std::vector<RecordTailLine> tail;
};

struct PresetData {
    int                format_version = 1;
    std::string        header_rest;  // the header line after the version, as written
    std::string        id;
    int                version = 1;
    std::string        name;
    DetectOptions      options;
    AnalyseOptions     analyse;
    std::vector<Block> blocks;
    KeptText           head;  // unknown lines right after the header line
    KeptText           kept;  // the `preset` line
};

// True when anything was kept as written (an unknown line, field or unreadable value).
bool HasKeptText(const ItemRules& rules);
bool HasKeptText(const PresetData& preset);

constexpr const char kRulesMagic[] = "RAVRULES";
constexpr const char kPresetMagic[] = "RAVPRESET";

// Parses a record. False (and `out` untouched) when the text does not start with
// "RAVRULES <n>" (leading blank lines / BOM allowed): a corrupt record reads as no rules.
// Never throws on bad input.
bool ParseItemRules(const std::string& text, ItemRules* out);
std::string SerializeItemRules(const ItemRules& rules);

// The same for a preset file ("RAVPRESET <n>").
bool ParsePreset(const std::string& text, PresetData* out);
std::string SerializePreset(const PresetData& preset);

// Exact equality of everything detection reads, plus on/off, marker and colour (kept text ignored).
bool SignalsEqual(const SignalSpec& a, const SignalSpec& b);
bool BlocksEqual(const std::vector<Block>& a, const std::vector<Block>& b);
// The fields the record writes (kept text and the derived AnalyseOptions::smooth_ms ignored).
bool OptionsEqual(const DetectOptions& a, const DetectOptions& b);
bool AnalyseEqual(const AnalyseOptions& a, const AnalyseOptions& b);

// ---- Names -----------------------------------------------------------------------------

// The bone part of a signal's name, from its bone references: "L heel+toe" (a point on
// several bones of one side), "L knee" (a joint angle: its middle bone), "L foot" (yaw
// heel -> toe of one side), else the first bone's label.
std::string SignalBoneLabel(const SignalSpec& spec);

// The signal's name: bone label + the quantity word from its menus. Position, vertical,
// from the floor = "height"; speed = "speed" / "vertical speed" / "horizontal speed" /
// "X speed"; joint angle = "bend" (+ " speed"); yaw = "turn" (+ " speed").
// E.g. "L heel+toe height", "L knee bend speed", "hips turn speed".
std::string SignalName(const SignalSpec& spec, const std::string& bone_label);

// "C"-locale number text: the shortest form that reads back exactly (non-finite = "0").
std::string FormatNumber(double v);
// Reads a whole "C"-locale number. False when `s` is not exactly one finite number.
bool ReadNumber(const std::string& s, double* out);

}  // namespace rav
