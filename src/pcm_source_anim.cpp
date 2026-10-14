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

#include "asset_cache.h"   // ReadAssetFileStamp — the loop period follows a re-export (11-7)
#include "asset_loader.h"  // ProbeAnimationDuration — CPU-only, GL-free (Story 4.2)

#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>  // INT_MAX — the "no track yet / lowest priority" selection sentinel (Story 4.5)
#include <cstring>
#include <string>
#include <vector>

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
        Probe();
    }

    PCM_source* Duplicate() override            { return new AnimSource(m_path.c_str()); }
    bool        IsAvailable() override           { return true; }
    const char* GetType() override               { return kSourceType; }
    const char* GetFileName() override           { return m_path.c_str(); }
    // A path change re-sizes the source — re-probe so GetLength stays correct.
    bool        SetFileName(const char* fn) override
    {
        m_path = fn ? fn : "";
        Probe();
        return true;
    }
    int         GetNumChannels() override         { return 0; }    // not audio
    double      GetSampleRate() override          { return 0.0; }  // <1.0 => silent
    // Real animation duration (FR9); 1.0 is now only the FALLBACK for a clip-less /
    // unparseable file — never return 0, which would be a degenerate item (AC3).
    double      GetLength() override
    {
        const double len = m_len.load(std::memory_order_relaxed);
        return len > 0.0 ? len : 1.0;
    }
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

    // Story 11-7 review: the loop period is this length, and the Tagging markers use the
    // file's clip duration, which the Tagging view re-reads when the file is re-exported.
    // Re-probe here too when the file's date or size changed (checked at most once a second;
    // the probe is the same parse as at creation). Main thread.
    void RefreshLengthIfChanged()
    {
        const double now = std::chrono::duration<double>(
                               std::chrono::steady_clock::now().time_since_epoch()).count();
        if (m_checked_at >= 0.0 && now - m_checked_at < 1.0) return;
        m_checked_at = now;
        if (ReadAssetFileStamp(m_path) != m_stamp) Probe();
    }

private:
    // The stamp is read BEFORE the parse, so a re-export during it is seen next time.
    void Probe()
    {
        m_stamp = ReadAssetFileStamp(m_path);
        m_len.store(ProbeAnimationDuration(m_path), std::memory_order_relaxed);
    }

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
    // Cached clip duration in seconds (0 => use fallback). Atomic: REAPER may read the
    // length off the main thread while a re-export re-probes it.
    std::atomic<double> m_len{0.0};
    AssetFileStamp      m_stamp;              // the file's version m_len was probed from
    double              m_checked_at = -1.0;  // RefreshLengthIfChanged's last check (s)
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

bool ReadPlayCandidate(MediaItem* item, int track_rank, PlayCandidate* out, std::string* out_path, bool refresh_length)
{
    if (!item || !out) return false;
    MediaItem_Take* tk = GetActiveTake(item);
    if (!tk) return false;
    PCM_source* src = GetMediaItemTake_Source(tk);
    if (!IsOurs(src)) return false;    // skip non-RAV (coexistence)
    const char* fn = src->GetFileName();
    if (!fn || !fn[0]) return false;   // no file: nothing to show
    PlayCandidate c;
    c.pos = GetMediaItemInfo_Value(item, "D_POSITION");
    c.len = GetMediaItemInfo_Value(item, "D_LENGTH");
    // D_STARTOFFS is the take's start-in-source: trimming the left edge reveals LATER
    // frames of the same clip (FR11), like native media. Source time then advances at
    // D_PLAYRATE (REAPER resizes the item itself when the rate changes in Item properties;
    // the source's length stays in source time), and that length is the loop period.
    c.start_offs = GetMediaItemTakeInfo_Value(tk, "D_STARTOFFS");
    c.rate       = GetMediaItemTakeInfo_Value(tk, "D_PLAYRATE");
    // A re-exported file: its new length (the Tagging view re-reads it too).
    if (refresh_length) {
        if (const char* t = src->GetType(); t && !std::strcmp(t, kSourceType))
            static_cast<AnimSource*>(src)->RefreshLengthIfChanged();
        else if (PCM_source* inner = src->GetSource(); inner && inner->GetType() && !std::strcmp(inner->GetType(), kSourceType))
            static_cast<AnimSource*>(inner)->RefreshLengthIfChanged();
    }
    c.clip_len   = src->GetLength();
    c.loop       = GetMediaItemInfo_Value(item, "B_LOOPSRC") != 0.0;
    c.muted      = GetMediaItemInfo_Value(item, "B_MUTE") != 0.0;
    // REAPER 7 lanes: on a fixed-lane track only the playing lanes count (C_LANEPLAYS 1 or
    // 2); -1 = a hidden, non-playing lane of a formerly fixed-lane track, on any track.
    MediaTrack*  tr    = GetMediaItem_Track(item);
    const bool   fixed = tr && static_cast<int>(GetMediaTrackInfo_Value(tr, "I_FREEMODE")) == 2;
    const double lp    = GetMediaItemInfo_Value(item, "C_LANEPLAYS");
    c.lane_plays = fixed ? (lp > 0.0) : !(lp < 0.0);
    c.lane       = fixed ? static_cast<int>(GetMediaItemInfo_Value(item, "I_FIXEDLANE")) : 0;
    c.track_rank = track_rank;
    *out = c;
    if (out_path) *out_path = fn;
    return true;
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
                          MediaTrack** out_track, MediaItem** out_item, bool* out_skipped)
{
    if (out_skipped) *out_skipped = false;
    ReaProject* proj = nullptr;  // current project
    // The heard play position while playing, edit cursor while stopped — so a stopped scrub
    // still drives the displayed frame (AC1); 11-6: in sync with the sound (RavPlayhead).
    const double pos = RavPlayhead(proj);

    // Story 11-7: every RAV item under the playhead is a candidate; PickPlayingItem
    // (play_item_plan.h) keeps the one REAPER plays: not muted, on a playing lane, the
    // topmost track (Story 4.5), then the upper lane, then the later start. Story 11-4:
    // one track only (Video view on a pinned track) = the same rule on that track, the
    // rule the video FX uses (video_timeline.cpp FindVideoItemAt), so Video view shows
    // what that track's FX renders. The clip time follows rate and loop (PlayClipTime).
    struct Found {
        MediaItem*  item;
        std::string path;
    };
    std::vector<PlayCandidate> cands;
    std::vector<Found>         found;
    auto consider = [&](MediaItem* it) {
        if (!it) return;
        // Cheap span test first: most items are not under the playhead; only spanning items
        // pay for the track rank.
        const double ip = GetMediaItemInfo_Value(it, "D_POSITION");
        const double il = GetMediaItemInfo_Value(it, "D_LENGTH");
        if (!(pos >= ip && pos < ip + il)) return;
        int rank = 0;
        if (!only_track) {
            // Topmost track = smallest 1-based IP_TRACKNUMBER (returned directly, not a
            // pointer-out). 0 (not found) / -1 (master) / null track => lowest priority.
            MediaTrack* tr = GetMediaItem_Track(it);
            rank = tr ? static_cast<int>(GetMediaTrackInfo_Value(tr, "IP_TRACKNUMBER")) : 0;
            if (rank <= 0) rank = INT_MAX;
        }
        PlayCandidate c;
        std::string   path;
        if (!ReadPlayCandidate(it, rank, &c, &path)) return;
        cands.push_back(c);
        found.push_back({it, std::move(path)});
    };
    if (only_track) {
        for (int i = 0, n = CountTrackMediaItems(only_track); i < n; ++i)
            consider(GetTrackMediaItem(only_track, i));
    } else {
        for (int i = 0, n = CountMediaItems(proj); i < n; ++i) consider(GetMediaItem(proj, i));
    }
    const int best = PickPlayingItem(cands, pos);
    if (best < 0) {
        // RAV items under the playhead, none playing (muted, or on a lane that does not
        // play): REAPER shows nothing there, so the viewer clears. Nothing at all: it holds
        // the last frame (AC1).
        if (out_skipped) *out_skipped = !cands.empty();
        return false;
    }

    out_path      = found[best].path;
    out_anim_time = PlayClipTime(cands[best], pos);
    if (out_track) *out_track = only_track ? only_track : GetMediaItem_Track(found[best].item);
    if (out_item) *out_item = found[best].item;
    return true;
}

}  // namespace rav
