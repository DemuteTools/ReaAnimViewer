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

// The project's store (nullptr = the current project). Empty for a project without one.
const SoundStore& GetSoundStore(ReaProject* proj);
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
// record removes the key.
bool ModifyItemSounds(MediaItem* item, const char* undo_desc, const std::function<bool(ItemSounds&)>& edit);

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
