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
#include <climits>  // INT_MAX — the "no track yet / lowest priority" selection sentinel (Story 4.5)
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

// Split one project line "key=value" at the FIRST '=' into (key, value), trimming
// a trailing CR/LF the project context may carry. Returns false — i.e. "ignore
// this line" — for a null line, a line with NO '=', or an EMPTY key. That single
// primitive gives LoadState its whole forward-compat contract for free (AC3):
// Reaper's native uppercase FILE "…" line has no '=' (ignored — Reaper owns it),
// a nested <…> block opener has no '=' (ignored), and any unknown future key
// simply isn't matched (ignored). ASCII/byte-wise, null-safe, no <sstream>.
bool SplitKeyValue(const char* line, std::string& key, std::string& value)
{
    if (!line) return false;
    const char* eq = std::strchr(line, '=');
    if (!eq || eq == line) return false;  // no '=' (native FILE/<…>), or empty key
    key.assign(line, (size_t)(eq - line));
    value.assign(eq + 1);
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
        value.pop_back();
    return true;
}

// Permanent on-disk type tag (architecture.md#D8): CreateFromType matches on it
// and (Epic 6) SaveState/LoadState recognize our sources by it — changing it
// after ship breaks every saved .rpp project. RAV-prefixed per the Story 1.1
// rename (the spike's throwaway tag was "FBXAV_ANIM").
constexpr const char kSourceType[] = "RAV_ANIM";

// Schema version for our key=value state lines (D9). SaveState writes it as
// `rav_ver=N`; LoadState ignores it today (a harmless unknown key) — it exists so
// the persisted format is versioned from day one and a future LoadState can branch
// on the schema without guessing (Story 6.1, fwd-compat).
constexpr int kStateVer = 1;

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
    // --- Per-item project persistence (D9, Story 6.1) ---
    // The file PATH rides Reaper's native FILE "…"/GetFileName/SetFileName mechanism
    // (the same one Story 6.3 relies on for cross-machine relink). SaveState/LoadState
    // additionally persist a versioned key=value line set in OUR source sub-chunk; the
    // file= line is DEFENSIVE (a load-time fallback), applied only-if-empty so the
    // native/relinked path is always authoritative (AC4 / NFR-R2 / 6.3-safe).
    void SaveState(ProjectStateContext* ctx) override
    {
        if (!ctx) return;  // no-throw null-guard (AR18); write ONLY our own lines (NFR-R2)
        // Build each line ourselves and emit via "%s" so a '%' in a path can't be
        // read as a format specifier (paths are data, never the AddLine format).
        ctx->AddLine("%s", ("rav_ver=" + std::to_string(kStateVer)).c_str());  // schema marker
        if (!m_path.empty())
            ctx->AddLine("%s", ("file=" + m_path).c_str());  // DEFENSIVE; native FILE wins (AC4)
        // Do NOT write the uppercase FILE line (Reaper's), nor time_offset/time_scale/
        // cam_* (deferred — native take state / global camera; Story 6.1 Task 5).
    }

    int LoadState(const char* firstline, ProjectStateContext* ctx) override
    {
        // firstline may be the <SOURCE RAV_ANIM opener OR the first inner content line
        // depending on the Reaper build — ApplyStateLine ignores anything that isn't
        // key=value, so both orderings are handled (the opener has no '=').
        ApplyStateLine(firstline);
        if (ctx) {
            char buf[4096];
            buf[0] = '\0';  // GetLine's contract only promises -1 at eof — don't read buf[0] uninitialized
            while (ctx->GetLine(buf, sizeof(buf)) != -1) {  // SDK: GetLine returns -1 at eof
                if (buf[0] == '>') break;                   // defensive: end of our sub-chunk
                ApplyStateLine(buf);
                buf[0] = '\0';                              // re-arm so a no-write return next loop is a safe no-op
            }
        }
        return 0;  // never -1 on an unknown/malformed line (AC3) — a bad line must not drop the item
    }
    void        Peaks_Clear(bool) override {}
    int         PeaksBuild_Begin() override { return 0; }
    int         PeaksBuild_Run() override   { return 0; }
    void        PeaksBuild_Finish() override {}

private:
    // Apply one persisted state line (D9, Story 6.1). Ignores any line that isn't
    // key=value (the native FILE line, a <…> opener, a blank line) and every key we
    // don't recognize (incl. rav_ver and any future key) — that IS the AC3 fwd-compat
    // contract. The ONLY honored key is file=, and ONLY when our path is still empty,
    // so Reaper's native/relinked path always wins (AC4, 6.3-safe). Never throws,
    // never reports failure — a parse miss is silently ignored.
    void ApplyStateLine(const char* line)
    {
        std::string key, value;
        if (!SplitKeyValue(line, key, value)) return;  // ignore non-key=value (AC3)
        if (key == "file") {
            // only-if-empty: a relinked project (6.3) has m_path already set by the
            // native SetFileName → our stale file= is skipped → native path is authoritative.
            if (m_path.empty() && !value.empty()) SetFileName(value.c_str());  // re-probes m_len
        }
        // every other key intentionally ignored (fwd-compat) — never return failure here.
    }

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

bool IsRavAnimSource(PCM_source* s) { return IsOurs(s); }

double ItemAnimTime(double pos, double item_pos, double start_offs)
{
    // Native left-trim: D_STARTOFFS is the take's start-in-source. animTime advances
    // from there, so trimming the left edge reveals LATER frames of the same clip
    // (FR11 native-media behavior) rather than restarting at frame 0. Take-level value.
    double at = (pos - item_pos) + start_offs;
    if (!(at >= 0.0)) at = 0.0; // low guard; the negated form also catches a non-finite
                              // D_STARTOFFS (NaN compares false → reset to 0, not propagated).
                              // RenderFrame (loop==false) clamps the HIGH end
                              // to the clip duration, so a deep startoffs past the clip
                              // end holds the last frame (AC3) — do NOT re-clamp to il here.
    return at;
}

// Read-only transport→current-item query (Story 4.3). Mirrors the Spike's
// CurrentAnimTime (spike/0-1-feasibility:src/spike_pcmsource.cpp) but ALSO returns
// the matched item's source path so the viewer can switch the displayed asset on a
// scrub onto a different item (AC2). Main-thread only, no-throw, READ-only (no
// Register — the boundary rule keeps that in plugin_main.cpp).
bool GetCurrentAnimItem(std::string& out_path, double& out_anim_time)
{
    return GetCurrentAnimItemOn(nullptr, out_path, out_anim_time, nullptr);
}

bool GetCurrentAnimItemOn(MediaTrack* only_track, std::string& out_path, double& out_anim_time,
                          MediaTrack** out_track)
{
    ReaProject* proj = nullptr;  // current project
    // Play cursor (continuous audio clock) while playing, edit cursor while stopped —
    // so a stopped scrub still drives the displayed frame (AC1).
    const bool playing = (GetPlayStateEx(proj) & 1) != 0;
    const double pos = playing ? GetPlayPosition2Ex(proj) : GetCursorPositionEx(proj);

    // Story 11-4 — one track only (Video view on a pinned track): the first spanning RAV
    // item in the track's item order, the rule the video FX uses (video_timeline.h
    // FindVideoItemAt), so Video view shows what that track's FX renders.
    if (only_track) {
        for (int i = 0, n = CountTrackMediaItems(only_track); i < n; ++i) {
            MediaItem* it = GetTrackMediaItem(only_track, i);
            if (!it) continue;
            const double ip = GetMediaItemInfo_Value(it, "D_POSITION");
            const double il = GetMediaItemInfo_Value(it, "D_LENGTH");
            if (pos < ip || pos >= ip + il) continue;
            MediaItem_Take* tk = GetActiveTake(it);
            if (!tk) continue;
            PCM_source* src = GetMediaItemTake_Source(tk);
            if (!IsOurs(src)) continue;
            const char* fn = src->GetFileName();
            if (!fn || !fn[0]) continue;  // no file: the FX skips it too (video_timeline.cpp)
            out_path = fn;
            out_anim_time = ItemAnimTime(pos, ip, GetMediaItemTakeInfo_Value(tk, "D_STARTOFFS"));
            if (out_track) *out_track = only_track;
            return true;
        }
        return false;
    }

    // Highest-priority spanning RAV item wins (Story 4.5): on overlap, the one on the
    // TOPMOST track (smallest 1-based IP_TRACKNUMBER) — mirroring Reaper's native video
    // compositing precedence. This is NOT an early-return: a higher-priority item may
    // appear LATER in CountMediaItems order, so scan EVERY item, keep the best candidate,
    // and emit the outputs for the WINNER after the loop. Ties (same track, or an
    // unreadable track number) keep the first walk-order item via strict < (deterministic,
    // no flicker — AC3). Null-guard every handle in the chain.
    MediaItem*      best    = nullptr;
    MediaItem_Take* best_tk = nullptr;
    double          best_ip = 0.0;
    int             best_tn = INT_MAX;  // smaller = higher priority; sentinel = none yet

    for (int i = 0, n = CountMediaItems(proj); i < n; ++i) {
        MediaItem* it = GetMediaItem(proj, i);
        if (!it) continue;
        const double ip = GetMediaItemInfo_Value(it, "D_POSITION");
        const double il = GetMediaItemInfo_Value(it, "D_LENGTH");
        if (pos < ip || pos >= ip + il) continue;          // not under the playhead
        MediaItem_Take* tk = GetActiveTake(it);
        if (!tk) continue;
        if (!IsOurs(GetMediaItemTake_Source(tk))) continue; // skip non-RAV (coexistence)

        // Highest-priority track = topmost = smallest 1-based IP_TRACKNUMBER.
        // GetMediaTrackInfo_Value(tr, "IP_TRACKNUMBER") returns the int DIRECTLY (not a
        // pointer-out attribute). 0 (not found) / -1 (master) / null track => lowest
        // priority (INT_MAX), never "wins as track 0".
        MediaTrack* tr = GetMediaItem_Track(it);
        int tn = tr ? (int)GetMediaTrackInfo_Value(tr, "IP_TRACKNUMBER") : 0;
        if (tn <= 0) tn = INT_MAX;

        // (!best ||) admits the FIRST spanning candidate unconditionally, so a lone item
        // whose track is unreadable (tn remapped to INT_MAX) is still selected — otherwise
        // INT_MAX < INT_MAX is false and a valid item under the playhead would be dropped.
        // The strict < then keeps the first walk-order item on a same-track tie (AC3).
        if (!best || tn < best_tn) {
            best_tn = tn; best = it; best_tk = tk; best_ip = ip;
        }
    }
    if (!best) return false;          // no RAV item under the playhead -> viewer holds (AC1)

    // Emit for the WINNER (best/best_tk/best_ip), not the first match. The per-item time
    // math is the SAME as 4.4 — only WHICH item feeds it changed.
    PCM_source* src = GetMediaItemTake_Source(best_tk);
    const char* fn  = src ? src->GetFileName() : nullptr;   // re-read; null-guard
    out_path = fn ? fn : "";                                // std::string assign LAST
    // Item-relative time from the take's start-in-source (left-trim) — ItemAnimTime,
    // shared with the video FX timeline (Story 11-2).
    const double off = GetMediaItemTakeInfo_Value(best_tk, "D_STARTOFFS");
    out_anim_time = ItemAnimTime(pos, best_ip, off);
    if (out_track) *out_track = GetMediaItem_Track(best);
    return true;
}

}  // namespace rav
