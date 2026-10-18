// SPDX-License-Identifier: MIT
//
// Library view (13-7a): the Libraries and Switch Groups lists (column 1) and the library tree (column 2).
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-7a
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.
//
// Seams for 13-7b (node panel, audition), 13-7c (Media Explorer drops) and 13-11 (missing
// files): the selection getters below and LibraryTreeHooks.

#pragma once

#ifdef _WIN32

#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

#include "library_tree_edit.h"
#include "sound_model.h"
#include "ui_theme.h"

namespace rav {

// Red "missing" and amber Unassigned (DECISIONS section 2), the mockup's --bad and --warn (13-7,
// audit A5; ui::kMissText / kMissName / kUnassignedName for the lighter text variants).
constexpr ImU32 kLibMissing = ui::kBad;
constexpr ImU32 kLibUnassigned = ui::kWarn;

void DrawLibrariesSection();
void DrawSwitchGroupsSection();
void DrawLibraryTree();
// Column 3 when several rows are selected: Type, Switch on, Put into, Make them default.
void DrawLibraryBatchPanel();

// The library shown in the tree ("" = none).
const std::string& SelectedLibraryUid();
// The selected rows of that library (pruned each frame against the visible rows).
const std::vector<LibraryRow>& SelectedLibraryRows();
// Selects one row of a library (and shows that library): clears the tree's search, reveals the
// row (an empty value row: its Switch too) and scrolls it into view.
void SelectLibraryRow(const std::string& lib_uid, const LibraryRow& row);
// 13-42: the same, then the Library view shown (from the Tagging view, the Sound assignment window, the
// Populate report): the library shown, the tree opened down to the row, the row selected and scrolled
// into view, its node's panel in column 3. An unknown library only switches the view.
void ShowInLibrary(const std::string& lib_uid, const LibraryRow& row);
// The node of the single selected row ("" = none, an empty value, or several rows).
std::string LibraryRowNode();
// 13-43: adds or removes one body part of a library's kit (the Kit... menu, the Roles card's chips), after the
// frame, one undo point "RAV: Set library kit" with its toast.
void ToggleLibraryKitPart(const std::string& lib_uid, const std::string& part);

// What other stories plug into the tree. Every member is optional.
struct LibraryTreeHooks {
    std::function<void(const std::string& lib, const std::string& node)> play;  // ▶ / Space
    std::function<void()>                                               stop;  // Esc / ▶ again
    std::function<bool(const std::string& node)>                        is_lit;  // sounding now
    // A Sound whose media is missing (13-11 / 13-7c: File sources). Item sources are checked
    // here (GUID not in the project) before this hook is asked.
    std::function<bool(const SoundNode& sound)> is_missing;
    // A drag payload type the tree accepts (13-7c: Media Explorer files), and what a drop does.
    std::string                                                                          media_payload_type;
    std::function<void(const std::string& lib, const LibraryRow& row, const void* data, int size)> media_drop;
    // 13-69: called every frame a media drag is over a target that takes it (before the drop), with
    // that target's row, so the drag's tooltip says what the drop makes there.
    std::function<void(const std::string& lib, const LibraryRow& row)> media_hover;
};
void SetLibraryTreeHooks(const LibraryTreeHooks& hooks);
// 13-17d: the tree has the focus (its rows walk with the arrow keys; REAPER must not take them).
bool LibraryTreeTakesArrows();
// The hooks set now (13-7b merges its play / stop / is_lit into them; 13-7c
// re-applies its media fields over what another story set).
const LibraryTreeHooks& GetLibraryTreeHooks();

// 13-23 (D9): the role keys a role map offers (RoleMenuKeys: the built-in families, then the user's
// custom roles), the custom roles re-read at most once a second.
const std::vector<std::string>& LibraryRoleKeys();
// How many of LibraryRoleKeys() are built-in (the custom ones follow).
size_t LibraryBuiltinRoleCount();

}  // namespace rav

#endif  // _WIN32
