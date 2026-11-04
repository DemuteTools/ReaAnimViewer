// SPDX-License-Identifier: MIT
//
// The tracks Populate places sounds on (pure, no REAPER). Story 13-13 named them after the Switch
// path; story 13-39 replaced its per-project option (`trk=`: Off / One per path / Folders) with a
// per-library Track template: an ordered row of levels (the library name, a Switch group, the
// Blend name, the Blend layer, the user's own fixed-text folder) and a "folders end" marker; the
// levels before it are folders, those after it join into the track name.
//
// The track path sits inside the Lands-in folder of the sound's node ("RAV Library" by default).
// The REAPER side finds or makes it (sound_tracks.h).

#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "sound_model.h"

namespace rav {

// Story 13-17 (N4): the separator a template joins the levels after "folders end" with ("Walk_Heel"): the
// store header's `sep=dash|dot|space` token; none (or another value) = "_", the default.
std::string TrackSeparatorOf(const SoundStore& store);
// The separators offered, in menu order: "_", "-", ".", " ".
const std::vector<std::string>& TrackSeparators();
// Writes the token ("_" removes it), the header's other tokens untouched. False when unchanged or
// `sep` is not one of TrackSeparators().
bool SetTrackSeparator(SoundStore& store, const std::string& sep);

// N1: a numbered name, game convention: no separator, two digits ("Heel01", "Thud02", "Heel100").
std::string NumberedName(const std::string& base, int n);

// One Switch on a Sound's path: the group and the value whose case holds the child on the path.
struct SwitchStep {
    std::string group_uid, group_name, value_uid, value_name;
};

// The Switch path of a Sound, from the root down (T1): each Switch ancestor's group and the value
// whose case holds the child on the path (Random and Blend ancestors give none). From the tree,
// not from an event's value. Empty when no Switch is on the path, when a Switch has the child on
// no value (Unassigned), or on a cycle. A missing group or value is named "?".
std::vector<SwitchStep> SwitchPathOf(const SoundStore& store, const SoundLibrary& lib, const std::string& sound_uid);

// One track level, from the top down. `folder` = the next level sits inside it.
struct TrackStep {
    std::string name;
    bool        folder = false;
    // Story 13-16 (B7): the track's hidden mark (track P_EXT, built from uids): the track is found
    // by it anywhere in the project. "" = found by name under its parent (the Lands-in folder).
    std::string mark;
};


// The child track a level matches (T3): trimmed, ASCII case ignored, the first one. -1 = none.
int MatchChildTrack(const std::vector<std::string>& child_names, const std::string& name);

// The Lands-in folder's child a File Sound's layer `layer` (1-based) lands on (13-11's layer
// tracks): the N-th child (never by name: a deleted "layer K" would give two layers one track),
// counting only the children that are no RAV Switch-tree folder (`switch_tree`, the library folders the
// Switch-named tracks sit in), so a Switch tree is never taken as a layer. -1 = none yet: a new
// last child "layer <next_number>" is made (`next_number` = those children + 1). With no
// Switch-tree folder this is the position 13-11 took.
int PickLayerChild(const std::vector<std::string>& child_names, const std::vector<bool>& switch_tree, int layer,
                   int* next_number = nullptr);

// ---- Story 13-16: Blend layer tracks, no overlapping placed items ------------------------------

// A Sound's Blend layer (B1, B2): the child of its nearest Blend ancestor on its path (a Sound, a
// Random or a Switch). Blends nested straight in Blends share the outermost one's folder: their
// layers sit flat beside the outer layers.
struct BlendLayer {
    bool        in_blend = false;
    std::string blend_uid;   // the Blend whose folder holds the layer (the outermost of a Blend-in-Blend chain)
    std::string blend_name;  // its name, "(unnamed)" when empty
    std::string layer_uid;   // the nearest Blend's child on the Sound's path
    std::string layer_name;  // its name trimmed, "(unnamed)" when empty; "Thud", "Thud02"... in the Blend's
                             // flat layer order when two layers share a name (13-17 N1)
};
BlendLayer LayerOf(const SoundLibrary& lib, const std::string& sound_uid);

// The hidden marks (B7), from uids only (the Lands-in value is the node's setting, not a track
// name): a template track (TemplateTrackPath); an NVK pile child (B5) under a template track, per
// source child track (its GUID); a spill child (B6, k >= 1) of the track marked `base`; a RAV
// track that has no mark (13-11's "layer N"), by its GUID.
std::string PileChildMark(const std::string& layer_mark, const std::string& source_track_guid);
std::string LaneMark(const std::string& base, int k);
std::string GuidTrackMark(const std::string& track_guid);
// The k of a spill child's mark (and its base's mark in `base`); 0 (base = the mark) otherwise.
int LaneOfMark(const std::string& mark, std::string* base);

// ---- Story 13-39: the Track template -----------------------------------------------------------

// A template level. Stored on the library (SoundLibrary::track_levels) as words: "lib", "g:<group uid>",
// "blend", "layer", "c:<text>", "end" (End = "folders end": the levels after it join into the track name).
enum class LevelKind { Library, Group, Blend, Layer, Custom, End };
struct TemplateLevel {
    LevelKind   kind = LevelKind::End;
    std::string arg;  // the group uid (Group), the folder's text (Custom)
};
// False for a word RAV does not know (or a "g:" / "c:" with nothing after it).
bool        ParseTemplateLevel(const std::string& word, TemplateLevel* out);
std::string TemplateLevelWord(const TemplateLevel& level);
// The known words in order, exactly one "end" (the first; appended last when none).
std::vector<std::string> CleanTemplate(const std::vector<std::string>& words);

// T1: the tree order of a library, used while no template is stored (by Populate and the chips alike):
// the library, the Switch groups in the order the tree first reaches them (a group nested k times on one
// path appears k times), "layer" when a Sound is in a Blend, "end". Never "blend" (the user may add it).
std::vector<std::string> DefaultTrackTemplate(const SoundStore& store, const SoundLibrary& lib);
// Per Switch group of `lib`: the most times it is nested on one path (its Switches from one up to the root).
std::map<std::string, int> TemplateGroupRepeats(const SoundLibrary& lib);
// The library's template: its stored one (CleanTemplate), else DefaultTrackTemplate.
std::vector<std::string> TrackTemplateOf(const SoundStore& store, const SoundLibrary& lib);
// True when `word` may be added to template `words` (the Add menu, AddTemplateLevel): a custom folder with
// text; a group while `words` holds it fewer times than the tree nests it (TemplateGroupRepeats, at least
// once); "lib", "blend", "layer" once. Never "end".
bool CanAddTemplateLevel(const SoundLibrary& lib, const std::vector<std::string>& words, const std::string& word);

// T2 / T4: a Sound's track path from template `words`: the Lands-in folder (no mark), then each level
// before "end" that has a value for the Sound as a folder, then the values the levels after "end"
// have, joined with `sep`, as the track that holds the sounds; nothing after "end": the last folder
// holds them. A level with no value is skipped (a Group: the n-th level of a group takes the n-th
// step of that group on the Switch path; Blend / Layer: outside any Blend). Empty (13-11's
// "layer N" track) when no level gives a value. Marks: folder i = "T|<lands>|<lib>|k0,..,ki", the
// joined track = "T|<lands>|<lib>|<folder keys>><keys after end>" (keys: "l", "g<group>:<value>",
// "b<blend uid>", "y<layer uid>", "c<encoded text>"): the same values give the same mark (a shared track).
std::vector<TrackStep> TemplateTrackPath(const std::vector<std::string>& words, const std::vector<SwitchStep>& path,
                                         const BlendLayer& layer, const std::string& lands_in, const std::string& lib_uid,
                                         const std::string& library_name, const std::string& sep = "_");
// The path Populate makes for a Sound of `lib` (T5): its template (TrackTemplateOf) under EffectiveLandsIn
// of the Sound, the store's separator. The second form takes the template already read (one per library).
std::vector<TrackStep> SoundTemplatePath(const SoundStore& store, const SoundLibrary& lib, const std::string& sound_uid);
std::vector<TrackStep> SoundTemplatePath(const SoundStore& store, const SoundLibrary& lib, const std::string& sound_uid,
                                         const std::vector<std::string>& words);

// T6: the template edits, on the library `lib_uid` of `store` (its tree order first when none is
// stored). The result equal to the tree order is stored as none. False when nothing changed.
// Move: the level at `from` takes the place of the one at `to`. Add: "c:<text>" (trimmed, refused
// empty) goes first, another word before "end" (refused when CanAddTemplateLevel says no). Remove: any
// level but "end". Reset: back to the tree order.
bool MoveTemplateLevel(SoundStore& store, const std::string& lib_uid, int from, int to);
bool AddTemplateLevel(SoundStore& store, const std::string& lib_uid, const std::string& word);
bool RemoveTemplateLevel(SoundStore& store, const std::string& lib_uid, int index);
bool ResetTrackTemplate(SoundStore& store, const std::string& lib_uid);

// The Final structure preview: the tracks the library's Sounds land on, in tree order, depth-first.
// Depth 0 = a Lands-in folder (no mark). `holds`: Sounds land on it (♪); `folder`: rows below it;
// `sounds`: the Sound uids whose track is it or below it. Sounds whose path is empty are left out.
// `why`: the level it comes from (MarkLevelLabel; "Lands in" for a Lands-in folder).
struct StructureRow {
    int                      depth = 0;
    std::string              name;
    std::string              mark;
    std::string              why;
    bool                     folder = false;
    bool                     holds = false;
    std::vector<std::string> sounds;
};
std::vector<StructureRow> TemplateStructure(const SoundStore& store, const SoundLibrary& lib);
// The level a template mark's track comes from, as the preview says it: "library", the Switch group's
// name, "Blend", "Layer", "your folder"; the joined track "name: <level> + <level>". "" for another mark.
std::string MarkLevelLabel(const SoundStore& store, const std::string& mark);

// Story 13-17 (N1): the name the Library gives the RAV track marked `mark`, from the store: a template
// track = its level's value (the library's name, a Switch value, the Blend's name, the layer's name
// -- LayerOf, "Thud02" for a repeat --, the folder's text), the joined track = its values joined by the
// store's separator; NVK pile child = the current name of its source child
// track (`track_name(mark)`: the name of the track carrying that mark, a GuidTrackMark here); spill
// child k = its base's name numbered ("Heel01"; a base RAV gives no name to, 13-11's "layer N",
// by its current name). "" = no Library name (a GuidTrackMark, an unknown mark, library, node,
// group or value): the track keeps its name.
std::string NameForMark(const SoundStore& store, const std::string& mark,
                        const std::function<std::string(const std::string& mark)>& track_name);

// N2: what to do with a marked track, from its current name `cur`, the name RAV recorded on it
// `given` ("" = none: a track made before 13-17, taken as RAV's) and its Library name `want`
// ("" = none: left as it is). A user-renamed track (given set, cur differs) is never touched.
// `rename`: set its name to `want`; `record`: record `want` as the name RAV gave.
struct TrackRename {
    bool rename = false;
    bool record = false;
};
TrackRename PlanTrackRename(const std::string& cur, const std::string& given, const std::string& want);

// An item's time span on a track lane; `id` names the item (-1 = none).
// 13-62h review (R1): `group` = the placement it is a piece of ("" = none known: PlacementGroup of its tag), and
// its fades (s), so two pieces of one placement crossfading at a Gap stay on one lane (CrossfadeButted).
struct LaneSpan {
    double      start = 0.0;
    double      end = 0.0;
    int         id = -1;
    std::string group;
    double      fade_in = 0.0, fade_out = 0.0;
};
// True when they overlap: items that only touch (end = start, within 1 us) do not, nor two pieces of one placement
// whose overlap is their crossfade (CrossfadeButted).
bool SpansOverlap(const LaneSpan& a, const LaneSpan& b);
// 13-62h review (R1): the geometry of a crossfade: they overlap (more than 1 us) by no more than the earlier one's
// fade out and the later one's fade in (within 1 us), the later one ending after the earlier one (not inside it).
bool MayCrossfade(const LaneSpan& a, const LaneSpan& b);
// MayCrossfade and both pieces of the same placement (same non-empty `group`): butted for the lane rules.
bool CrossfadeButted(const LaneSpan& a, const LaneSpan& b);
// The placement a placed item's tag names (its owner, rule id, event time and node: every piece of one placement
// carries the same ones); "" for an empty owner.
std::string PlacementGroup(const PlacedTag& tag);
// True when `s` overlaps no span of `lane` but those with id `exclude_id` (the item itself).
bool FitsLane(const std::vector<LaneSpan>& lane, const LaneSpan& s, int exclude_id = -1);
// The first lane where `s` fits (B6), -1 = none (a new lane).
int PickLane(const std::vector<std::vector<LaneSpan>>& lanes, const LaneSpan& s, int exclude_id = -1);
// A track's items spilled onto lanes (B6, the first overlap): `items` in position order (ties: as
// given), each on the first lane of `lanes` (the existing ones, filled in) where it fits, new lanes
// added at the end. Returns each item's lane index.
std::vector<int> SpillOnLanes(std::vector<LaneSpan> items, std::vector<std::vector<LaneSpan>>* lanes);

// B6 on a plain RAV track (marked `base_mark`, no spill child yet): `items` = the items on it but
// the new one, `tks` their tags' tk ("" = not placed by RAV). False (nothing moves) when `item`
// fits there. Else the first overlap: the items RAV placed there (tk == base_mark) and `item`
// spill onto children 1..`lanes` (SpillOnLanes); the user's items, and those the user moved
// there, stay (B6b, B7b). A mover's new tk = LaneMark(base_mark, lane + 1).
struct SpillPlan {
    int              lanes = 0;      // spill children to make, k = 1..lanes
    std::vector<int> lane_of;        // per entry of `items`: its lane (0-based), -1 = stays on the track
    int              item_lane = 0;  // the new item's lane (0-based)
};
bool PlanSpill(const std::vector<LaneSpan>& items, const std::vector<std::string>& tks, const std::string& base_mark,
               const LaneSpan& item, SpillPlan* out);
// B6 once spilled: the k of the spill child `item` goes on, given the existing ones (`ks`, their
// items `lanes`, same order, ks ascending): the first that fits, else a new one, the highest k + 1
// (`*made` true).
int PickLaneK(const std::vector<int>& ks, const std::vector<std::vector<LaneSpan>>& lanes, const LaneSpan& item,
              bool* made);

// ---- Story 13-39d: Overlaps (Inside / Beside) ---------------------------------------------------

// True when the track family of `base_mark` (a template track, its NVK pile child, or a spill lane of either)
// belongs to a library set to Beside (SoundLibrary::overlaps_beside). False for any other mark (13-11's
// "layer N", a GuidTrackMark: Inside, O6) or a library not found.
bool OverlapsBesideFor(const SoundStore& store, const std::string& base_mark);
// O2, Beside: where `item` goes: 0 = it stays on the base track (`base` = the spans there) when it fits;
// else the k of the first existing spill track that fits (`ks` ascending, `lanes` their spans, same order);
// else a new one, the highest k + 1 (`*made` true). Nothing already placed moves.
int PickBesideK(const std::vector<LaneSpan>& base, const std::vector<int>& ks,
                const std::vector<std::vector<LaneSpan>>& lanes, const LaneSpan& item, bool* made);
// The template track of an NVK pile child mark ("<track mark>|K|<source guid>"), else "" (not a pile child).
std::string PileChildBase(const std::string& mark);
// Folder depth math over the project's tracks' I_FOLDERDEPTH (`depths`, track order), as FolderChildren (ravlib.h):
// the index of the folder holding the track at `idx` (-1 = top level).
int ParentOfDepth(const std::vector<int>& depths, int idx);
// A new track as the next sibling of the track at `anchor` (same parent): inserted at `at` (after the anchor's last
// descendant when it is a folder), with depth `new_depth`; the track at `changed` (an index before `at`, -1 = none)
// takes `changed_depth`, so every folder still closes where it did. A folder with nothing in it (the last track)
// becomes a plain track.
struct SiblingInsert {
    int at = -1;
    int new_depth = 0;
    int changed = -1;
    int changed_depth = 0;
};
SiblingInsert SiblingAfterDepths(const std::vector<int>& depths, int anchor);

// B7b: where a placed item sits, from its tag's tk and whether it is on the track RAV uses for that
// mark (FindTrackByMark: a copy of a RAV track carrying the same mark is not it).
enum class PlacedSpot {
    NotRav,       // tk "": placed on a track RAV did not make (its source's own): never "moved"
    OnRavTrack,   // where RAV put it
    MovedByUser,  // the user put it elsewhere: kept there by a replace, the follow and the spill
};
// `base` (optional) = the mark of its track family (a spill child's tk resolves to its base).
PlacedSpot PlacedSpotOf(const std::string& tk, bool on_tk_track, std::string* base = nullptr);

}  // namespace rav
