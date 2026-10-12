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

#include <string>
#include <vector>

#include "sound_model.h"

namespace rav {

enum class TrackLayout { Off, Flat, Tree };

// The project's layout: the store header's `trk=flat|tree` token (none or another value = Off).
TrackLayout TrackLayoutOf(const SoundStore& store);
// Writes the token (Off removes it), the header's other tokens untouched. False when unchanged.
bool SetTrackLayout(SoundStore& store, TrackLayout layout);

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
};

// The track path for a Switch path (T2, T5, T11): the Lands-in folder, a folder named after the
// library, then one track "Run-Shoes-Concrete" (Flat) or one folder per value, the last one the
// items' track (Tree). Empty (keep today's track) when Off or with no Switch on the path. An
// empty name reads "(unnamed)".
std::vector<TrackStep> TrackPathFor(const std::vector<SwitchStep>& path, TrackLayout layout,
                                    const std::string& lands_in, const std::string& library_name);

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

}  // namespace rav
