// SPDX-License-Identifier: MIT
//
// See video_fx_track.h.

#include "video_fx_track.h"

#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "console_log.h"
#include "video_fx_api.h"
#include "video_camera_params.h"

namespace rav {
namespace {

std::string Lower(const char* s)
{
    std::string out(s ? s : "");
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool StartsWith(const std::string& s, const std::string& prefix)
{
    return s.compare(0, prefix.size(), prefix) == 0;
}

bool IsVideoFx(MediaTrack* track, int fx)
{
    char buf[1024] = {};
    // fx_ident is type-specific: for a CLAP it names the plug-in file and/or id.
    if (TrackFX_GetNamedConfigParm(track, fx, "fx_ident", buf, sizeof(buf))) {
        const std::string ident = Lower(buf);
        if (ident.find(Lower(RAV_VIDEO_FX_CLAP_ID)) != std::string::npos) return true;
        if (ident.find(Lower(RAV_VIDEO_FX_FILE)) != std::string::npos) return true;
    }
    // Original name (not the user's rename): "CLAP: RAV Video FX (Demute)". The
    // "(" after the name keeps "RAV Video FX Spike (...)" and the like out.
    buf[0] = '\0';
    if (!TrackFX_GetNamedConfigParm(track, fx, "fx_name", buf, sizeof(buf)) || !buf[0]) {
        if (!TrackFX_GetFXName(track, fx, buf, sizeof(buf))) return false;
    }
    const std::string name = Lower(buf);
    const std::string base = Lower(RAV_VIDEO_FX_NAME);
    return name == base || StartsWith(name, "clap: " + base + " (") || StartsWith(name, base + " (");
}

}  // namespace

int FindVideoFxOnTrack(MediaTrack* track)
{
    if (!track) return -1;
    const int count = TrackFX_GetCount(track);
    for (int i = 0; i < count; ++i) {
        if (IsVideoFx(track, i)) return i;
    }
    return -1;
}

void RefreshVideoFxPictures()
{
    const int tracks = CountTracks(nullptr);
    for (int t = 0; t < tracks; ++t) {
        MediaTrack* tr = GetTrack(nullptr, t);
        const int fx = FindVideoFxOnTrack(tr);
        if (fx < 0) continue;
        // Any new value will do; step and wrap so it never sticks at the top.
        const double v = TrackFX_GetParamNormalized(tr, fx, vcam::kRefreshParam);
        TrackFX_SetParamNormalized(tr, fx, vcam::kRefreshParam, std::fmod((v > 0.0 ? v : 0.0) + 1.0 / 64.0, 1.0));
    }
}

AddVideoFxResult AddVideoFxToTrack(MediaTrack* track, int* out_index)
{
    if (out_index) *out_index = -1;
    if (!track) return AddVideoFxResult::NotInstalled;

    const int existing = FindVideoFxOnTrack(track);
    if (existing >= 0) {
        if (out_index) *out_index = existing;
        return AddVideoFxResult::AlreadyThere;
    }

    // Full browser name first (exact plug-in), then the bare name.
    static const char* const kNames[] = {
        "CLAP:" RAV_VIDEO_FX_NAME " (" RAV_VIDEO_FX_VENDOR ")",
        "CLAP:" RAV_VIDEO_FX_NAME,
    };
    for (const char* name : kNames) {
        const int idx = TrackFX_AddByName(track, name, false, -1);  // -1: always add a new instance
        if (idx < 0) continue;
        if (IsVideoFx(track, idx)) {
            if (out_index) *out_index = idx;
            LogInfo("video FX added to a track at chain index %d (%s)", idx, name);
            return AddVideoFxResult::Added;
        }
        // REAPER matched another plug-in by name: take it back out.
        TrackFX_Delete(track, idx);
    }
    return AddVideoFxResult::NotInstalled;
}

void AddVideoFxToSelectedTracks()
{
    const int selected = CountSelectedTracks(nullptr);
    if (selected <= 0) {
        ShowMessageBox("Select a track first: the video FX goes on the track of the animation.",
                       "RAV: Add video FX", 0);
        return;
    }

    // Only the tracks without the FX change: no empty undo point when all have it.
    std::vector<MediaTrack*> missing;
    for (int i = 0; i < selected; ++i) {
        MediaTrack* track = GetSelectedTrack(nullptr, i);
        if (track && FindVideoFxOnTrack(track) < 0) missing.push_back(track);
    }
    if (missing.empty()) return;

    int added = 0;
    int not_installed = 0;
    Undo_BeginBlock2(nullptr);
    for (MediaTrack* track : missing) {
        switch (AddVideoFxToTrack(track, nullptr)) {
            case AddVideoFxResult::Added:        ++added; break;
            case AddVideoFxResult::AlreadyThere: break;
            case AddVideoFxResult::NotInstalled: ++not_installed; break;
        }
    }
    Undo_EndBlock2(nullptr, added > 1 ? "RAV: Add video FX to tracks" : "RAV: Add video FX to track",
                   UNDO_STATE_FX);

    if (not_installed > 0) ShowVideoFxNotInstalledMessage();
}

void ShowVideoFxNotInstalledMessage()
{
    ShowMessageBox("REAPER does not know the RAV Video FX plug-in (" RAV_VIDEO_FX_FILE ").\n\n"
                   "It is installed with ReaAnimViewer. If you just installed or updated it, restart "
                   "REAPER. Otherwise reinstall it: ReaPack users update the ReaAnimViewer package; "
                   "Demute Reaper Toolkit users click Run on the ReaAnimViewer card. Then restart REAPER.",
                   "RAV: Add video FX", 0);
}

}  // namespace rav
