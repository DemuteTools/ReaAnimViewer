// SPDX-License-Identifier: MIT
//
// Switch-named tracks (story 13-13, Antho 2026-10-12, pure, no REAPER): with nested Switches
// (LocomotionType > FeetType > Material) a placed sound lands on a track named after the Switch
// path that chose it, so the arrange says why it is there. A per-project option (the sound store
// header's `trk=` token): Off (the source track, or the Lands-in folder's layer track, as before),
// one track per path ("Run-Shoes-Concrete"), or folders by Switch (Run > Shoes > Concrete).
//
// The track path sits inside the Lands-in folder ("RAV Library" for a project library), under a
// folder named after the library. The REAPER side finds or makes it (sound_tracks.h).

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "sound_model.h"

namespace rav {

enum class TrackLayout { Off, Flat, Tree };

// The project's layout: the store header's `trk=flat|tree` token (none or another value = Off).
TrackLayout TrackLayoutOf(const SoundStore& store);
// Writes the token (Off removes it), the header's other tokens untouched. False when unchanged.
bool SetTrackLayout(SoundStore& store, TrackLayout layout);

// Story 13-17 (N4): the separator One per path joins Switch values with ("Run_Shoes_Concrete"): the
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
// name). A library folder; a Switch path track; a Blend folder (no Switch path); a layer track
// under the track marked `parent`; an NVK pile child (B5) under a layer track, per source child
// track (its GUID); a spill child (B6, k >= 1) of the track marked `base`; a RAV track that has
// no mark (13-11's "layer N"), by its GUID.
std::string LibraryMark(const std::string& lands_in, const std::string& lib_uid);
std::string BlendMark(const std::string& lands_in, const std::string& lib_uid, const std::string& blend_uid);
std::string LayerMark(const std::string& parent, const std::string& layer_uid);
std::string PileChildMark(const std::string& layer_mark, const std::string& source_track_guid);
std::string LaneMark(const std::string& base, int k);
std::string GuidTrackMark(const std::string& track_guid);
// The k of a spill child's mark (and its base's mark in `base`); 0 (base = the mark) otherwise.
int LaneOfMark(const std::string& mark, std::string* base);

// The track path of a Sound (B3, B4, 13-13 T2): with a Switch path (layout not Off), the Lands-in
// folder, the library folder and the Switch path tracks, then, inside a Blend, the last one as a
// folder and the layer track. Without a Switch path (or Off), inside a Blend: the Lands-in
// folder, the library folder, a folder named after the Blend, the layer track. Empty (today's
// track) outside any Blend with no Switch path. Every level but the Lands-in folder is marked.
// One per path joins the value names with `sep` (TrackSeparatorOf, 13-17).
std::vector<TrackStep> SoundTrackPath(const std::vector<SwitchStep>& path, const BlendLayer& layer, TrackLayout layout,
                                      const std::string& lands_in, const std::string& lib_uid,
                                      const std::string& library_name, const std::string& sep = "_");

// Story 13-17 (N1): the name the Library gives the RAV track marked `mark`, from the store: library
// folder = the library's name; Switch path track = its values joined by the store's separator (One
// per path) or its last value (Folders); Blend folder = the Blend's name; layer track = the layer's
// name (LayerOf, "Thud02" for a repeat); NVK pile child = the current name of its source child
// track (`track_name(mark)`: the name of the track carrying that mark, a GuidTrackMark here); spill
// child k = its base's name numbered ("Heel01"; a base RAV gives no name to, 13-11's "layer N",
// by its current name). "" = no Library name (a GuidTrackMark, an unknown mark, library or node):
// the track keeps its name.
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
struct LaneSpan {
    double start = 0.0;
    double end = 0.0;
    int    id = -1;
};
// True when they overlap: items that only touch (end = start, within 1 us) do not.
bool SpansOverlap(const LaneSpan& a, const LaneSpan& b);
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
