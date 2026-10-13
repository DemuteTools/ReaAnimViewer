// SPDX-License-Identifier: MIT
//
// 13-48 (13-32, 13-33, 13-42): the Library tree's view state and navigation, pure (host-tested by
// tests/library_tree_nav_test.cpp). The folded containers as the text kept in REAPER ExtState
// (view state: never the project, never an undo point), Collapse all, and the row a rule's
// assignment opens the Library on.

#pragma once

#include <set>
#include <string>

#include "library_tree_edit.h"  // LibraryRow
#include "sound_model.h"
#include "sound_switch.h"  // ValueLayers

namespace rav {

// 13-32: one library's folded containers as text: the uids in `closed` that are containers of `lib`
// (Random / Blend / Switch nodes, the root included), sorted, joined by ','. A uid holding ',' or a
// control character is left out. "" = nothing folded.
std::string EncodeTreeFolds(const SoundLibrary& lib, const std::set<std::string>& closed);
// The uids of a text EncodeTreeFolds wrote (empty pieces dropped; uids of nodes since deleted are
// harmless: the next write prunes them).
std::set<std::string> DecodeTreeFolds(const std::string& text);

// 13-33: Collapse all = every container of `lib` folded except its root (the root's children show,
// each folded). Expand all = none folded.
std::set<std::string> CollapsedTreeFolds(const SoundLibrary& lib);
// Whether Expand all / Collapse all would change anything (`closed` may hold other libraries' uids).
bool TreeCanExpandAll(const SoundLibrary& lib, const std::set<std::string>& closed);
bool TreeCanCollapseAll(const SoundLibrary& lib, const std::set<std::string>& closed);
// Applies one of them to `closed`, leaving the other libraries' uids alone.
void SetTreeFoldsAll(const SoundLibrary& lib, bool collapse, std::set<std::string>* closed);

// 13-33: a fold-all scope (ui::FoldScopeBar: the foldable cards of one panel) shows Expand all /
// Collapse all only with at least two foldable cards; Expand all is on while one is folded, Collapse
// all while one is open.
struct FoldScopeButtons {
    bool shown = false, can_expand = false, can_collapse = false;
};
FoldScopeButtons FoldScopeButtonsFor(int foldable, int open);

// 13-42: the row the Library opens on for an assignment of `lib` with `layers` (a rule's values):
// from the root, each Switch follows the value the layers give (else its default, as ResolveEvent
// does), down to the first node that is not a Switch (a Random, a Blend or a Sound: what the rule
// plays), shown on its row (RowOfNode: a value row under a Switch). A value that holds no sound
// stops on that empty value row; a value no longer in the group, an unknown group or a cycle stops on
// the Switch.
LibraryRow RevealRowFor(const SoundStore& store, const SoundLibrary& lib, const ValueLayers& layers);

}  // namespace rav
