// SPDX-License-Identifier: MIT
//
// Epic 13 (D20, story 13-2) -- the sound model's REAPER storage. FROZEN after 13-2.
//
// - The project store (Switch Groups, libraries) lives in the .rpp as a projectconfig chunk
//   `<RAVSOUNDS` ... `>` (sound_record.h grammar), saved in undo states too: a store edit is an
//   undo point (UNDO_STATE_MISCCFG). An empty store writes no chunk (D1).
// - Each animation item's sound state lives in its RAV take's P_EXT:RAV_SOUNDS (D2).
// - Each item RAV places carries P_EXT:RAV_SND (D4). Never on a folder item.
// - One gesture = one undo point: wrap multi-step work in SoundUndoScope, and pass a null
//   undo_desc to the Modify calls inside it.

#pragma once

#ifdef _WIN32

#include <cstdint>
#include <functional>
#include <string>

#include "reaper_api.h"
#include "sound_model.h"

namespace rav {

constexpr const char kItemSoundsKey[] = "P_EXT:RAV_SOUNDS";
constexpr const char kPlacedTagKey[] = "P_EXT:RAV_SND";
// 13-11 (B9): the mark of a muted User Library source item ("1") an older build imported; its
// copies are placed unmuted. Populate no longer makes such items (Antho 2026-10-12).
constexpr const char kLibSourceTagKey[] = "P_EXT:RAV_LIBSRC";
constexpr size_t     kItemSoundsMaxBytes = 1u << 20;

// Registers the projectconfig extension and resolves this module's extra functions (D7).
void InitSoundProject(reaper_plugin_info_t* rec);
// Unregisters it (plugin unload, reverse order).
void QuitSoundProject();

// 13-19: a gesture's live-item cache (PlaceBatch turns it on and off; nested calls count). On, an item
// checked live once (IsLiveItem, and the Read / Write calls below) is not checked again with
// ValidatePtr2, which walks the project's items. Every item the gesture deletes must be forgotten
// (ForgetLiveItem) before it is deleted. Off, every check is ValidatePtr2 on the current project.
void LiveItemsBatch(bool on);
void ForgetLiveItem(MediaItem* item);
bool IsLiveItem(MediaItem* item);

// The project's store (nullptr = the current project). Empty for a project without one.
const SoundStore& GetSoundStore(ReaProject* proj);
// Moves each time a store is replaced (an edit, a project load or close): a reference taken from
// GetSoundStore is stale once it has moved.
uint64_t SoundStoreGeneration();
// Edits the store: `edit` returns false to change nothing. A changed store marks the project
// dirty and, with a non-null `undo_desc`, records one undo point; null = no undo point (inside a
// caller's SoundUndoScope). False when nothing changed.
bool ModifySoundStore(ReaProject* proj, const char* undo_desc, const std::function<bool(SoundStore&)>& edit);
// Story 13-17 (N3): called after every store change, in the same gesture; it returns true when it
// changed tracks (RefreshRavTrackNames), and the undo point then records the tracks too.
void SetSoundStoreEditHook(bool (*hook)(ReaProject* proj));

// The animation item's sound state (empty when none or unreadable).
ItemSounds ReadItemSounds(MediaItem* item);
// Same contract as ModifySoundStore, for the item's P_EXT:RAV_SOUNDS (UNDO_STATE_ITEMS). An empty
// record removes the key. 13-59 review: `too_large` (optional) is set when the edited record was
// refused for its size (kItemSoundsMaxBytes): nothing written, the caller says so to the user.
bool ModifyItemSounds(MediaItem* item, const char* undo_desc, const std::function<bool(ItemSounds&)>& edit,
                      bool* too_large = nullptr);

// The tag on a placed item. False when it has none (or it does not read).
bool ReadPlacedTag(MediaItem* item, PlacedTag* out);
bool WritePlacedTag(MediaItem* item, const PlacedTag& tag);
bool ClearPlacedTag(MediaItem* item);

// One undo point around a gesture: Undo_BeginBlock2 now, Undo_EndBlock2(proj, desc, UNDO_STATE_ALL)
// when it ends.
class SoundUndoScope {
public:
    SoundUndoScope(ReaProject* proj, const char* desc);
    ~SoundUndoScope();
    SoundUndoScope(const SoundUndoScope&) = delete;
    SoundUndoScope& operator=(const SoundUndoScope&) = delete;

private:
    ReaProject* proj_;
    std::string desc_;
};

// The dev check action (D10, RAV_SOUND_STORE_CHECK): an empty store gets a test library (one undo
// point "RAV: Add test sound library"), else the store's summary is shown.
void RunSoundStoreCheck();

}  // namespace rav

#endif  // _WIN32
