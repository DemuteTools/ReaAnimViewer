// SPDX-License-Identifier: MIT
//
// Render Current (spec 11-fb-16): the Video panel's third render button renders the clip
// shown, from the master mix, whatever the project's render source is. Pure, header-only,
// no REAPER.
//
// REAPER's RENDER_SETTINGS (GetSetProjectInfo) holds the render source in a few bits:
//   &(1|2)  stems (master mix when both are clear)
//   &8      region render matrix
//   &32     selected media items       &64     selected media items via master
//   &128    selected tracks via master
//   &8192   razor edit areas           &16384  razor edit areas via master
// Master mix = all of these cleared. Every other bit (multichannel, mono, embed options,
// 2nd pass...) is the user's and is kept.

#pragma once

namespace rav {

constexpr int kRenderSourceMask = 1 | 2 | 8 | 32 | 64 | 128 | 8192 | 16384;

// `settings` with its source set to the master mix, every other bit kept.
constexpr int RenderSettingsAsMasterMix(int settings)
{
    return settings & ~kRenderSourceMask;
}

// The settings to write back after the dialog: the source bits as saved (only the source was
// RAV's), every other bit as the user left it in the dialog (`now`).
constexpr int RenderSettingsRestored(int now, int saved)
{
    return (now & ~kRenderSourceMask) | (saved & kRenderSourceMask);
}

// RENDER_TAILFLAG bit 1 = tail on custom time bounds: cleared while the dialog is set to the
// clip, so the render stops at the clip's end (not in the next clip).
constexpr int kRenderTailCustomBounds = 1;
constexpr int RenderTailFlagNoCustomTail(int tail_flag)
{
    return tail_flag & ~kRenderTailCustomBounds;
}

// The project's render bounds and source, saved before Render Current prefills the Render
// dialog and put back once it closes.
struct SavedRenderBounds {
    double bounds_flag = 0.0;  // RENDER_BOUNDSFLAG (0 = custom time range)
    double start = 0.0;        // RENDER_STARTPOS
    double end = 0.0;          // RENDER_ENDPOS
    double settings = 0.0;     // RENDER_SETTINGS (only its source bits come back)
    double tail_flag = 0.0;    // RENDER_TAILFLAG
};

}  // namespace rav
