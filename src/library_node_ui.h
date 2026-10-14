// SPDX-License-Identifier: MIT
//
// Library view (13-7b): the selected node's panel (column 3).
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-7b
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.
//
// 13-7b: the node panel (play, type, values, volume, Mix, Blend lanes), the Sound panel (Source
// waveform with hand edits, Replace, Variations, Detection) and the Edit overlay. Seams for
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

// ---- 13-7b additions ----------------------------------------------------------------------------

// 13-20 / 13-23 / 13-25 (Edit only): a Sound's parts drawn over its variations -- the peaks coloured by
// part (Combined: one waveform and a ribbon under it; Split: one lane per value), excluded stretches
// hatched, the cuts (a grab at the bottom: dashed proposed, solid moved by hand, amber "?" unsure) and the
// sync points (a flag at the top: dashed detected, solid moved by hand), and the zone selected.
struct WaveParts {
    const SoundData*         data = nullptr;
    std::vector<ImU32>       colors;  // one per divide value
    std::vector<std::string> names;   // one per divide value
    bool                     split = false;  // one lane per value
    int                      solo = -1;      // Combined: only this part bright (-1 = all)
    double                   sel_start = 0.0, sel_len = 0.0;  // the zone selected (length 0 = none)
};

// A waveform with its variations framed (the Sound panel, Edit, the Media Explorer preview).
struct WaveformOptions {
    ImVec2 size = ImVec2(0.0f, 90.0f);  // x 0 = the available width
    double view_start = 0.0;            // source seconds shown
    double view_len = 0.0;              // 0 = the whole source
    double source_len = 0.0;
    bool   gestures = false;            // hand edits: trim, dB drag, Alt-click off, S split
    bool   select = false;              // click selects (Ctrl-click adds) instead of playing
    bool   ruler = false;               // a ms / s ruler on top
    double threshold_db = std::numeric_limits<double>::quiet_NaN();  // a draggable line when finite
    // Middle-drag pans (REAPER's hand scroll). 13-45: the wheel and the zoom keys are the caller's
    // (view_nav.h ViewNavInput, the same for every timeline).
    bool   pan = false;
    std::vector<bool> solid_edges;      // per frame: drawn solid (a clamped edge); may be empty
    // Antho 2026-10-12: per frame, its fades as placed (drawn as REAPER draws an item's); empty = none.
    // With `gestures`, a handle at each top corner drags the fade, Alt-click resets it.
    std::vector<FrameFades> fades;
    // The frames' numbers in mono 11 (Edit's large waveform) instead of mono 9 (the panel's).
    bool   big_numbers = false;
    // 13-20 / 13-23 / 13-25: the parts drawn, with their gestures (drag a cut or a sync flag, a
    // horizontal drag selects a zone); null = none.
    const WaveParts* parts = nullptr;
    // 13-54: the Sound whose parts' margins (pre-roll / tail around each part, PartPieces) are drawn when
    // `parts` is null (the panel): what a part leaves out is drawn as outside a variation. With `parts`,
    // its data is used.
    const SoundData* margins = nullptr;
    // 13-28: per frame, the gain from Normalize shown at the top right of its box (0 = none; empty = no
    // labels) and whether it clips (drawn in --bad).
    std::vector<double> gain_db;
    std::vector<bool>   gain_clip;
    // 13-41: the Sound forced to mono: one lane, the channel (or L + R) heard (MonoOf).
    MonoMode mono = MonoMode::Off;
    // 13-61: Edit's edit cursor (source seconds, < 0 = none): where the last click landed, drawn as a line.
    double cursor_s = -1.0;
};

struct WaveformEvents {
    bool   reading = false;     // the peaks are still being read
    int    clicked = -1;        // a frame clicked (no Alt, no drag)
    bool   ctrl = false;        // ... with Ctrl held
    bool   clicked_empty = false;  // a click outside every frame
    int    toggled = -1;        // Alt-click on a frame
    int    double_clicked = -1; // a frame double-clicked (-2 = empty space)
    double double_at = 0.0;     // its source time
    int    trim = -1;           // an edge drag: the frame and its new stretch
    double trim_start = 0.0, trim_len = 0.0;
    bool   trim_done = false;   // released (write it)
    int    db_frame = -1;       // a vertical drag inside a frame: its new dB
    double db = 0.0;
    bool   db_done = false;
    bool   threshold_live = false, threshold_done = false;
    double threshold_db = 0.0;
    bool   right_clicked = false;
    double scroll_s = 0.0;      // a middle-drag: move view_start by this
    int    fade_frame = -1;     // a fade handle drag: the frame and its own fades (s, < 0 = the default)
    double fade_in = -1.0, fade_out = -1.0;
    bool   fade_done = false;   // released (write it)
    int    fade_reset = -1;     // Alt-click on a fade handle: that frame's side back to the default
    int    fade_reset_side = 0; // -1 fade in, +1 fade out
    ImVec2 min, max;            // the waveform's rectangle
    // 13-20 / 13-23 / 13-25 (with `parts`).
    double click_at = 0.0;      // the source time of `clicked`
    double right_at = 0.0;      // the source time right-clicked
    int    cut_frame = -1, cut_k = -1;     // a cut dragged: its frame, its index and its time
    double cut_t = 0.0;
    bool   cut_done = false;
    int    sync_frame = -1, sync_part = -1;  // a sync flag dragged: its frame, its part and its time
    double sync_t = 0.0;
    bool   sync_done = false;
    bool   zone_live = false, zone_done = false;  // a zone being selected (a horizontal drag)
    double zone_start = 0.0, zone_len = 0.0;
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
// nothing when no part is selected).
void RequestLibraryEditKey(int shortcut_id);

}  // namespace rav

#endif  // _WIN32
