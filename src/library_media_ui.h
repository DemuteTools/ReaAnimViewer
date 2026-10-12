// SPDX-License-Identifier: MIT
//
// Library view (13-7c): the Media Explorer (column 4).
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-7c
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.
//
// Search over REAPER's media databases (media_db.h, loaded on a worker), a waveform preview with
// the variations split on silence (peaks read on a worker), REAPER preview playback, and the
// uses (drag onto the tree, double-click, "Use for ...") of library_media_plan.h, each one undo
// point with the Library toast.

#pragma once

#ifdef _WIN32


namespace rav {

void DrawMediaExplorerPanel();

// Resolves PlayPreview / StopPreview / PCM_Source_CreateFromFile (missing = no preview, no peaks).
void InitLibraryMedia(void* (*get_func)(const char* name));
// Unload: stops the preview, cancels and joins the peak reader, stops the database load.
void QuitLibraryMedia();
// Stops the Media Explorer's preview (if any).
void StopMediaPreview();

}  // namespace rav

#endif  // _WIN32
