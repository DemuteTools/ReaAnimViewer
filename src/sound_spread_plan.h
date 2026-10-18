// SPDX-License-Identifier: MIT
//
// Spread selection to marker type (13-68 v2, pure): the selection turned into a library that Populate
// places. The selected items are read in blocks like NVK (items overlapping in time = one pile = one
// variation with its layers; piles apart or touching = variations); the library (category Attached,
// named after the marker type) gets a Random root of the piles: a one-item pile = a Sound (an item of
// 2+ takes = a Takes Sound), a layered pile = a Blend of Sounds whose offsets keep the layers' snaps
// apart as cut. Each Sound syncs on its item's snap (Slice start + the snap as offset), so the placed
// copy's snap point lands on the marker. Add to stack makes the root a Blend of the existing content
// and a new layer. No REAPER: host-tested (tests/sound_spread_plan_test.cpp). The gesture:
// sound_spread_selection.h.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "sound_attach_plan.h"
#include "sound_model.h"

namespace rav {

// One selected item as a Sound: its facts, its name (its take's), its Sound data (read by the gesture).
struct SpreadSource {
    AttachItem  facts;
    std::string name;
    SoundData   sound;
    bool        folder = false;  // a folder item (an NVK pile): its pile is this one Sound (PileSoundData)
    // Its active take's playrate: its snap (item seconds) is snap x playrate into the source, where its copy (played
    // at rate 1, as auditioned) is synced (Antho 2026-10-18: a 1.335 layer's copies landed 16.7 ms late).
    double      playrate = 1.0;
};

// The piles of the selection: OverlapGroups with touching items apart, each pile's members top first
// (lowest track, then earliest position); piles in time order.
std::vector<std::vector<int>> SpreadPiles(const std::vector<AttachItem>& items);

// A Sound synced on its item's snap: Slice start, the snap (s, item time) as its Offset (ms).
void SyncFromSnap(SoundData& d, double snap_s);

// A folder item's Sound (an NVK pile, placed by Populate as its child items): source = the folder item,
// copies share the variation number (linked_copies): TakeMarkers mode with one variation per slice when its
// top member has take markers (`slices` >= 2: every layer plays its own k-th slice), else Takes mode with
// one variation per take when its children hold `takes` >= 2 (every layer plays take k), else Whole.
// `len` = the folder item's length (s).
SoundData PileSoundData(const std::string& folder_guid, int takes, double len, const std::vector<Slice>& slices = {});

// The nodes of one layer: nodes[0] = a Random named `name` (parent ""), one child per pile in time
// order: a pile holding a folder item = that item's Sound alone (its first folder item, top first; the
// other items of the pile are its layers); a one-item pile = its Sound; a layered pile = a Blend with
// linked variations (its layers play the same variation number) named after its top item holding one
// Sound per member, each child's offset = its snap after the top item's (BlendChildOffsetMs), kept on
// events with a role too. Every Sound is synced on its item's snap. Empty when `items` is.
std::vector<SoundNode> SpreadLayerNodes(const std::vector<SpreadSource>& items, const std::string& name,
                                        const std::function<std::string()>& new_uid);

// The Attached library of a marker type: the first with the attached flag and that `attached_type`
// (SameMarkerType), or, for one with no stored type, that name. Null when none.
SoundLibrary*       FindAttachedLibrary(SoundStore& store, const std::string& type);
const SoundLibrary* FindAttachedLibrary(const SoundStore& store, const std::string& type);

// The library's content replaced by `nodes` (nodes[0] = the new root, parent "").
void SetLibraryContent(SoundLibrary& lib, std::vector<SoundNode> nodes);

// Add to stack: `layer` (SpreadLayerNodes) becomes a child of the root Blend at `offset_ms`. A root
// that is not a Blend is wrapped first: a new Blend root `blend_uid` named after the library holding
// the old root (offset 0) then the layer. No root: the layer becomes the root (its offset 0).
void AddStackLayer(SoundLibrary& lib, std::vector<SoundNode> layer, double offset_ms, const std::string& blend_uid);

// Add to stack: the name of the next layer: "Layer 2" when the root is not a Blend (the existing content
// is layer 1), "Layer k+1" when it is a Blend of k children.
std::string StackLayerName(const SoundLibrary& lib);

// Add to stack: the new layer's offset (s) = `anchor` (the selection's top item's snap, project s)
// minus the nearest of `markers` (project s; tie: the earlier one). 0 when there is none.
double StackLayerOffset(double anchor, const std::vector<double>& markers);

// The source zone: where each selected original goes, after `project_end` (+1 s), their relative
// positions kept (PromoteCopyPosition from the earliest of `positions`).
std::vector<double> SourceZonePositions(double project_end, const std::vector<double>& positions);

// Antho 2026-10-18: the first pile of the selection is not copied: it moves onto the selected marker and stays
// attached to that event (it follows it; Populate leaves that event alone and places copies on the others). Its
// draws, so the copies avoid repeating it: for each Sound of `lib` whose source item is a key of `played` (item
// GUID, any case), the variation that item plays; for each Random above it, the child on its path.
std::vector<SoundDraw> PlayedDraws(const SoundLibrary& lib, const std::vector<std::pair<std::string, int>>& played);

// Add to stack (Antho 2026-10-18): the pile of `piles` whose top item's snap is nearest `t` (the stack's event, project
// s): the one the user put against the stack, attached there. -1 when there is none.
int NearestPile(const std::vector<AttachItem>& items, const std::vector<std::vector<int>>& piles, double t);

// What a run did, for its toast.
struct SpreadRunCounts {
    std::string type;                  // the marker type (= the library's name)
    bool        all_instances = false;
    bool        add_layer = false;     // Add to stack
    bool        reused = false;        // the type's Attached library existed
    int         selected_markers = 1;  // RAV markers selected (the first in time is used)
    int         variations = 0;        // piles of the selection
    int         layers = 0;            // items of the selection
    int         animations = 0;        // animation items assigned (pool members included)
    int         events = 0;            // events Populate handled
    int         placed = 0, replaced = 0, kept = 0;  // Populate's counts
    std::string skip_note;             // "2 skipped: placed by RAV" ("" = none)
};
// The report: "Used FS L (2 markers selected)" first when several, then e.g. "FS L (this animation):
// library FS L, 3 variations (6 items), 2 animations, 16 sounds placed" (zero counts left out).
std::string SpreadRunReport(const SpreadRunCounts& c);

}  // namespace rav
