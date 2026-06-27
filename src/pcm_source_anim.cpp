// SPDX-License-Identifier: MIT
//
// PCM_source factory: makes animation files first-class Reaper media (Story 4.1).
// Registration + a working source object ONLY — the source reports a placeholder
// length (real duration = Story 4.2), maps no playhead→frame (= Story 4.3), and
// loads no asset / touches no GL here.
//
// Mechanism proven by Spike 0 (spike/0-1-feasibility:src/spike_pcmsource.cpp),
// lifted to production with the boundary rules applied: rec->Register lives in
// plugin_main (not here), GetType is the permanent "RAV_ANIM" tag, .dae added.

#include "pcm_source_anim.h"

#include "asset_loader.h"  // ProbeAnimationDuration — CPU-only, GL-free (Story 4.2)

#include <cctype>
#include <cstring>
#include <string>

namespace rav {
namespace {

// Case-insensitive compare of two NUL-terminated ASCII strings (extensions).
bool IExtEq(const char* a, const char* b)
{
    while (*a && *b) {
        if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b)) return false;
        ++a; ++b;
    }
    return !*a && !*b;
}

// True iff fn's FINAL extension is one of ours, case-insensitive. Null-safe: a
// null or extension-less name is not ours, so the factory declines it (AC2).
bool HasAnimExt(const char* fn)
{
    if (!fn) return false;
    const char* dot = std::strrchr(fn, '.');
    if (!dot) return false;
    return IExtEq(dot, ".glb") || IExtEq(dot, ".gltf")
        || IExtEq(dot, ".fbx") || IExtEq(dot, ".dae");
}

// Permanent on-disk type tag (architecture.md#D8): CreateFromType matches on it
// and (Epic 6) SaveState/LoadState recognize our sources by it — changing it
// after ship breaks every saved .rpp project. RAV-prefixed per the Story 1.1
// rename (the spike's throwaway tag was "FBXAV_ANIM").
constexpr const char kSourceType[] = "RAV_ANIM";

// Minimal non-audio PCM_source: 0 channels + GetSampleRate()<1.0 mark it silent,
// yet a dropped file still creates a real timeline item (Spike 0). It holds the
// path and a cached real duration (Story 4.2) — GetLength reports the animation's
// length so the item is correctly sized; state/peaks remain stubs.
class AnimSource : public PCM_source {
public:
    // Eager CPU-only probe: Reaper calls GetLength() immediately after creating the
    // item, so resolve the duration once at construction and cache it (no re-parse
    // per GetLength call). GL-free, no-throw — safe with no GL context on drop.
    explicit AnimSource(const char* path) : m_path(path ? path : "")
    {
        m_len = ProbeAnimationDuration(m_path);
    }

    PCM_source* Duplicate() override            { return new AnimSource(m_path.c_str()); }
    bool        IsAvailable() override           { return true; }
    const char* GetType() override               { return kSourceType; }
    const char* GetFileName() override           { return m_path.c_str(); }
    // A path change re-sizes the source — re-probe so GetLength stays correct.
    bool        SetFileName(const char* fn) override
    {
        m_path = fn ? fn : "";
        m_len = ProbeAnimationDuration(m_path);
        return true;
    }
    int         GetNumChannels() override         { return 0; }    // not audio
    double      GetSampleRate() override          { return 0.0; }  // <1.0 => silent
    // Real animation duration (FR9); 1.0 is now only the FALLBACK for a clip-less /
    // unparseable file — never return 0, which would be a degenerate item (AC3).
    double      GetLength() override              { return m_len > 0.0 ? m_len : 1.0; }
    int         PropertiesWindow(HWND) override   { return 0; }
    void        GetSamples(PCM_source_transfer_t* block) override { if (block) block->samples_out = 0; }
    void        GetPeakInfo(PCM_source_peaktransfer_t*) override {}
    void        SaveState(ProjectStateContext*) override {}                        // stub — D9 / Epic 6
    int         LoadState(const char*, ProjectStateContext*) override { return 0; } // stub — D9 / Epic 6
    void        Peaks_Clear(bool) override {}
    int         PeaksBuild_Begin() override { return 0; }
    int         PeaksBuild_Run() override   { return 0; }
    void        PeaksBuild_Finish() override {}

private:
    std::string m_path;
    double      m_len = 0.0;  // cached clip duration in seconds (0 => use fallback)
};

// --- pcmsrc_register_t callbacks (C-style entries Reaper drives → no-throw, D5/AR18) ---

// Reaper recreates a source from its saved type tag. Ours only iff the tag matches.
PCM_source* CreateFromType(const char* type, int /*priority*/)
{
    try {
        if (type && !std::strcmp(type, kSourceType)) return new AnimSource("");
    } catch (...) {}  // bad_alloc must not escape into Reaper (AR18)
    return nullptr;
}

// Reaper offers a dropped/imported file. We claim it iff its extension is ours,
// else return nullptr so Reaper handles foreign files normally — never hijack (AC2).
PCM_source* CreateFromFile(const char* filename, int /*priority*/)
{
    try {
        if (HasAnimExt(filename)) return new AnimSource(filename);
    } catch (...) {}  // bad_alloc must not escape into Reaper (AR18)
    return nullptr;
}

// Enumerate our extensions for Reaper's file filters. Set the description on
// i==0 only; nullptr thereafter tells Reaper to reuse the last description.
const char* EnumFileExtensions(int i, const char** descptr)
{
    switch (i) {
    case 0: if (descptr) *descptr = "ReaAnimViewer animation"; return "glb";
    case 1: if (descptr) *descptr = nullptr;                   return "gltf";
    case 2: if (descptr) *descptr = nullptr;                   return "fbx";
    case 3: if (descptr) *descptr = nullptr;                   return "dae";
    default: return nullptr;
    }
}

// The single registration object. plugin_main passes &g_reg to Register / -Register
// with the IDENTICAL pointer (NFR-R3, AR15) — see PcmSourceRegistration().
pcmsrc_register_t g_reg = { &CreateFromType, &CreateFromFile, &EnumFileExtensions };

// True iff s (or the inner source it wraps) is one of ours — i.e. GetType() is the
// permanent production tag "RAV_ANIM" (kSourceType; NOT the Spike's "FBXAV_ANIM" —
// copying that string would make this silently never match). Reaper can wrap our
// source in a section/reverse take, so check the wrapped inner GetSource() too.
// Null-safe on s, the inner source, and every GetType() return — a raw vtable
// deref is SEH/UB, not catchable (D5/AR18).
bool IsOurs(PCM_source* s)
{
    if (!s) return false;
    const char* t = s->GetType();
    if (t && !std::strcmp(t, kSourceType)) return true;
    PCM_source* inner = s->GetSource();
    if (!inner) return false;
    const char* it = inner->GetType();
    return it && !std::strcmp(it, kSourceType);
}

}  // namespace

pcmsrc_register_t* PcmSourceRegistration() { return &g_reg; }

// Read-only transport→current-item query (Story 4.3). Mirrors the Spike's
// CurrentAnimTime (spike/0-1-feasibility:src/spike_pcmsource.cpp) but ALSO returns
// the matched item's source path so the viewer can switch the displayed asset on a
// scrub onto a different item (AC2). Main-thread only, no-throw, READ-only (no
// Register — the boundary rule keeps that in plugin_main.cpp).
bool GetCurrentAnimItem(std::string& out_path, double& out_anim_time)
{
    ReaProject* proj = nullptr;  // current project
    // Play cursor (continuous audio clock) while playing, edit cursor while stopped —
    // so a stopped scrub still drives the displayed frame (AC1).
    const bool playing = (GetPlayStateEx(proj) & 1) != 0;
    const double pos = playing ? GetPlayPosition2Ex(proj) : GetCursorPositionEx(proj);

    // First RAV item spanning the playhead wins (single-item / first-match; overlap +
    // track-priority is Story 4.5 — do NOT generalise this walk). Null-guard every
    // handle in the chain; the std::string assign is done LAST, only after a match.
    for (int i = 0, n = CountMediaItems(proj); i < n; ++i) {
        MediaItem* it = GetMediaItem(proj, i);
        if (!it) continue;
        const double ip = GetMediaItemInfo_Value(it, "D_POSITION");
        const double il = GetMediaItemInfo_Value(it, "D_LENGTH");
        if (pos < ip || pos >= ip + il) continue;
        MediaItem_Take* tk = GetActiveTake(it);
        if (!tk) continue;
        PCM_source* src = GetMediaItemTake_Source(tk);
        if (!IsOurs(src)) continue;

        // Matched. GetFileName() already returns m_path (no re-probe) — null-guard it.
        const char* fn = src->GetFileName();
        out_path = fn ? fn : "";
        // Native left-trim: D_STARTOFFS is the take's start-in-source. animTime advances
        // from there, so trimming the left edge reveals LATER frames of the same clip
        // (FR11 native-media behavior) rather than restarting at frame 0. Take-level value.
        const double off = GetMediaItemTakeInfo_Value(tk, "D_STARTOFFS");
        double at = (pos - ip) + off;
        if (!(at >= 0.0)) at = 0.0; // low guard; the negated form also catches a non-finite
                                  // D_STARTOFFS (NaN compares false → reset to 0, not propagated).
                                  // RenderFrame (loop==false) clamps the HIGH end
                                  // to the clip duration, so a deep startoffs past the clip
                                  // end holds the last frame (AC3) — do NOT re-clamp to il here.
        out_anim_time = at;
        return true;
    }
    return false;
}

}  // namespace rav
