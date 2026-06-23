// SPDX-License-Identifier: MIT
//
// THROWAWAY SPIKE (Spike 0 / Story 0-1) — NOT production code, do not merge to main.

#include "spike_pcmsource.h"

#include <cctype>
#include <cstring>
#include <string>

#include <windows.h>

#include "reaper_api.h"      // PCM_source, pcmsrc_register_t, transport/item API
#include "spike_loader.h"    // duration parse (CPU-only, no GL)

namespace spike {
namespace {

bool IExtEq(const char* a, const char* b)
{
    while (*a && *b) {
        if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b)) return false;
        ++a; ++b;
    }
    return !*a && !*b;
}

bool HasAnimExt(const char* fn)
{
    if (!fn) return false;
    const char* dot = std::strrchr(fn, '.');
    if (!dot) return false;
    return IExtEq(dot, ".fbx") || IExtEq(dot, ".glb") || IExtEq(dot, ".gltf");
}

double ParseDuration(const char* fn)
{
    if (!fn || !*fn) return 0.0;
    Model m;
    std::string err;
    if (LoadModel(fn, m, err) && m.clip.duration > 0.0f) return m.clip.duration;
    return 0.0;
}

// Minimal non-audio PCM_source: reports a timeline length (= animation duration) so
// Reaper makes a correctly-sized item; produces no audio. (Spike-grade: SaveState/
// peaks/etc. are stubs — real persistence is Epic 4/6.)
class AnimSource : public PCM_source {
public:
    explicit AnimSource(const char* fn) : m_fn(fn ? fn : "") { m_len = ParseDuration(m_fn.c_str()); }

    PCM_source* Duplicate() override { return new AnimSource(m_fn.c_str()); }
    bool        IsAvailable() override { return true; }
    const char* GetType() override { return "FBXAV_ANIM"; }
    const char* GetFileName() override { return m_fn.c_str(); }
    bool        SetFileName(const char* newfn) override { m_fn = newfn ? newfn : ""; m_len = ParseDuration(m_fn.c_str()); return true; }
    int         GetNumChannels() override { return 0; }      // not audio
    double      GetSampleRate() override { return 0.0; }     // <1.0 => silent
    double      GetLength() override { return m_len > 0.0 ? m_len : 1.0; }
    int         PropertiesWindow(HWND) override { return 0; }
    void        GetSamples(PCM_source_transfer_t* block) override { if (block) block->samples_out = 0; }
    void        GetPeakInfo(PCM_source_peaktransfer_t*) override {}
    void        SaveState(ProjectStateContext*) override {}
    int         LoadState(const char*, ProjectStateContext*) override { return 0; }
    void        Peaks_Clear(bool) override {}
    int         PeaksBuild_Begin() override { return 0; }
    int         PeaksBuild_Run() override { return 0; }
    void        PeaksBuild_Finish() override {}

private:
    std::string m_fn;
    double      m_len = 0.0;
};

PCM_source* CreateFromType(const char* type, int)
{
    if (type && !std::strcmp(type, "FBXAV_ANIM")) return new AnimSource("");
    return nullptr;
}

PCM_source* CreateFromFile(const char* filename, int)
{
    return HasAnimExt(filename) ? new AnimSource(filename) : nullptr;
}

const char* EnumFileExtensions(int i, const char** descptr)
{
    switch (i) {
    case 0: if (descptr) *descptr = "ReaAnimViewer animation"; return "fbx";
    case 1: if (descptr) *descptr = nullptr; return "glb";
    case 2: if (descptr) *descptr = nullptr; return "gltf";
    default: return nullptr;
    }
}

pcmsrc_register_t g_reg = { &CreateFromType, &CreateFromFile, &EnumFileExtensions };

// Reaper may wrap our source (section/pooled); unwrap one level when checking type.
bool IsOurs(PCM_source* s)
{
    if (!s) return false;
    if (s->GetType() && !std::strcmp(s->GetType(), "FBXAV_ANIM")) return true;
    PCM_source* inner = s->GetSource();
    return inner && inner->GetType() && !std::strcmp(inner->GetType(), "FBXAV_ANIM");
}

}  // namespace

void RegisterPcmSrc(int (*reg)(const char*, void*)) { if (reg) reg("pcmsrc", &g_reg); }
void UnregisterPcmSrc(int (*reg)(const char*, void*)) { if (reg) reg("-pcmsrc", &g_reg); }

bool CurrentAnimTime(double& outAnimTimeSec)
{
    ReaProject* proj = nullptr;  // current project
    const bool playing = (GetPlayStateEx(proj) & 1) != 0;
    const double pos = playing ? GetPlayPosition2Ex(proj) : GetCursorPositionEx(proj);

    const int n = CountMediaItems(proj);
    for (int i = 0; i < n; ++i) {
        MediaItem* it = GetMediaItem(proj, i);
        if (!it) continue;
        const double ip = GetMediaItemInfo_Value(it, "D_POSITION");
        const double il = GetMediaItemInfo_Value(it, "D_LENGTH");
        if (pos < ip || pos >= ip + il) continue;
        MediaItem_Take* tk = GetActiveTake(it);
        if (!tk) continue;
        if (IsOurs(GetMediaItemTake_Source(tk))) {
            double at = pos - ip;
            if (at < 0.0) at = 0.0;
            outAnimTimeSec = at;
            return true;
        }
    }
    return false;
}

}  // namespace spike
