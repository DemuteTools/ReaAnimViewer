// SPDX-License-Identifier: MIT
//
// Library view (13-7b): the selected node's panel (column 3).
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-7b
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.
//
// 13-7b: the node panel (play, type, values, volume, Mix, Blend lanes), the Sound panel (Source
// waveform, Replace, Variations, Detection) and the Edit overlay (13-62i: every Sound's zones). Seams for
// 13-7c: DrawWaveform, NodePanelHooks.

#pragma once

#ifdef _WIN32

#include <functional>
#include <limits>
#include <string>
#include <vector>

#include <imgui.h>

#include "sound_entries.h"  // FrameFades
#include "sound_model.h"

namespace rav {

void DrawNodePanel();
// Enters a value typed in a Volume / Detection fader whose panel is not drawn this frame (a Ctrl /
// Shift-click on another tree row shows the batch panel instead): one undo point on its own node.
// Called before column 3 draws; a fader left behind longer than a frame is dropped unwritten.
void SettleNodePanelFaders();
// The main menu's Library tab: a "Sound Edit" card with the Edit window's ⚙ Options rows (one shared list, the
// same prefs: per user, no undo point).
void DrawSoundEditMenuSection();

// ---- 13-7b additions ----------------------------------------------------------------------------

// 13-62 / 13-62h: a divided Sound's parts as the Edit window's Lane view headers read them (LaneHeaders): their
// colours and names, the Lane view on, the selected part ("" = none), whether Play hears each (Mute / Solo; empty = all
// heard), the header column's width as the user set it (logical px; 0 = the default 160).
struct WaveParts {
    const SoundData*         data = nullptr;
    std::vector<ImU32>       colors;  // one per divide value
    std::vector<std::string> names;   // one per divide value
    bool                     split = false;  // one lane per value
    std::string              sel_part;
    std::vector<bool>        heard;
    float                    head_px = 0.0f;
};

// A read-only waveform with its variations framed (the Sound panel, the Media Explorer preview). 13-62i: no edit
// gesture (P4: the Edit window edits a Sound's zones, zone_wave_ui.h).
struct WaveformOptions {
    ImVec2 size = ImVec2(0.0f, 90.0f);  // x 0 = the available width
    double view_start = 0.0;            // source seconds shown
    double view_len = 0.0;              // 0 = the whole source
    double source_len = 0.0;
    bool   ruler = false;               // a ms / s ruler on top
    // Antho 2026-10-12: per frame, its fades as placed (drawn as REAPER draws an item's); empty = none.
    std::vector<FrameFades> fades;
    // The frames' numbers in mono 11 instead of mono 9.
    bool   big_numbers = false;
    // 13-41: the Sound forced to mono: one lane, the channel (or L + R) heard (MonoOf).
    MonoMode mono = MonoMode::Off;
};

struct WaveformEvents {
    bool   reading = false;        // the peaks are still being read
    int    clicked = -1;           // a frame clicked (released where pressed)
    bool   ctrl = false;           // ... with Ctrl held
    bool   clicked_empty = false;  // a click outside every frame
    bool   right_clicked = false;
    ImVec2 min, max;               // the waveform's rectangle
    double click_at = 0.0;         // the source time of `clicked`
    double right_at = 0.0;         // the source time right-clicked
    // The Edit window's Lane view (13-62h: its zone waveform fills these for LaneHeaders): the lane header column on
    // the left of the lanes (104 px .. half the box; head_narrow under 116 px: no gain fader), each lane's top and
    // height. head_w 0 = no column (Combined).
    float  head_x = 0.0f, head_w = 0.0f;
    bool   head_narrow = false;
    float  lanes_y = 0.0f, lane_h = 0.0f;
};

WaveformEvents DrawWaveform(const char* id, const std::string& path, const std::vector<SoundFrame>& frames,
                            const std::vector<int>& selection, double playhead_s, const WaveformOptions& options);

// What 13-7c plugs into the Sound panel. Unset: "Show in Media Explorer" greyed. A Media
// Explorer drop on the node panel goes to LibraryTreeHooks' media_drop (library_tree_ui.h); a
// drop on a Sound's Source waveform is decoded by decode_media and replaces its audio.
struct NodePanelHooks {
    std::string media_payload_type;
    std::function<bool(const void* data, int size, SoundSource* source, std::vector<SoundFrame>* frames)> decode_media;
    std::function<void(const SoundSource& source)> show_in_media;
};
void SetNodePanelHooks(const NodePanelHooks& hooks);

// Decision 9: E (no modifier) while a Sound panel shows in the focused Library view, and
// Space / Esc / E while the Edit overlay is open (13-63: S is free, the split is a Shortcuts row); 13-45: the view zoom / scroll keys
// (view_nav.h) while the Edit overlay has the focus.
bool LibraryNodeTakesKey(unsigned vk, bool ctrl, bool shift, bool alt);
// Story 6-5-10: the Edit overlay is open and has the keyboard, so an Esc is its own (it clears the
// zone, stops Play, or closes the overlay): the clip list, open over the view, waits for the next.
bool LibraryEditUsesEsc();
// 13-61: the Edit overlay is open and has the keyboard: the Shortcuts rows of the Sound Edit window (the
// trims, ShortcutContext::LibraryEdit) run on their keys.
bool LibraryEditKeysActive();
// 13-61: a trim row's key ran (kShortcutEditTrimLeft / Right / Zone; 13-63: or kShortcutEditSplit): the Edit
// overlay does it on its next frame (one undo point), or says why it cannot. Ignored while the overlay is closed.
// 13-62: kShortcutEditSolo / Mute toggle the selected part's Solo / Mute there (view state, no undo point;
// nothing when no part is selected). 13-62F: kShortcutEditNextVar / PrevVar select the next / previous variation,
// the edit cursor at its start, the view centred on it (PlanEditNav; view state, no undo point). 13-62h:
// kShortcutEditHeal heals the selected zones, kShortcutEditLoop toggles Loop (view state).
// 13-67a: kShortcutEditExclSolo solos the selected part alone (again on the only soloed part: unsolo; view state),
// kShortcutEditSnapOffset sets the selected zone's snap offset at the edit cursor (one undo point),
// kShortcutEditPrevStop moves the edit cursor back one stop (time selection / zone start, file start last).
// 13-74: kShortcutEditGainUp / Down move the selected zones' gain one notch (1 dB; notches in a row: one undo point).
void RequestLibraryEditKey(int shortcut_id);
// 13-74: the Edit waveform was under the mouse last frame and this wheel (Ctrl / Shift / Alt) is a gain row's
// (Shortcuts: Ctrl+Shift+wheel by default): the accelerator hook keeps it from REAPER.
bool LibraryEditClaimsWheel(bool horizontal, bool ctrl, bool shift, bool alt);

}  // namespace rav

#endif  // _WIN32
