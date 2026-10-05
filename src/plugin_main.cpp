// SPDX-License-Identifier: MIT
//
// Reaper extension entry point. Phase 0: registers a single action that
// opens an empty WGL viewer window. No mesh loading, no transport sync.

#include "console_log.h"
#include "detection_measure.h"
#include "tag_markers.h"  // Story 10-4: Apply's marker functions
#include "pcm_source_anim.h"
#include "reaper_actions.h"
#include "reaper_api.h"
#include "self_update.h"
#include "video_fx_host.h"
#include "video_fx_track.h"
#include "video_timeline.h"
#include "video_shots.h"
#include "shortcuts.h"
#include "viewer_window.h"

namespace rav {
namespace {

constexpr const char kCommandName[] = "RAV_OPEN_VIEWER";
constexpr const char kActionDesc[]  = "RAV: Open Viewer";

// Story 11-1 -- the video FX actions.
constexpr const char kAddVideoFxName[]     = "RAV_VIDEOFX_ADD";
constexpr const char kAddVideoFxDesc[]     = "RAV: Add video FX to selected track";
constexpr const char kTestPatternName[]    = "RAV_VIDEOFX_TEST_PATTERN";
constexpr const char kTestPatternDesc[]    = "RAV: Video FX test pattern";

// Story 11-2 -- the video FX background option (viewer background / transparent).
constexpr const char kVideoBgName[] = "RAV_VIDEOFX_TRANSPARENT_BG";
constexpr const char kVideoBgDesc[] = "RAV: Video FX transparent background (selected tracks)";
int                  g_video_bg_id    = 0;
gaccel_register_t    g_video_bg_accel = {};

// Story 11-3 -- click-based test hooks for the shots (11-4/11-5 call the same helpers).
constexpr const char kAddShotName[]  = "RAV_VIDEOFX_ADD_SHOT";
constexpr const char kAddShotDesc[]  = "RAV: Video FX: add shot at edit cursor";
constexpr const char kShowShotsName[] = "RAV_VIDEOFX_SHOW_SHOTS";
constexpr const char kShowShotsDesc[] = "RAV: Video FX: show shots of selected track";
constexpr const char kOutputAngleName[] = "RAV_VIDEOFX_SET_OUTPUT_ANGLE";
constexpr const char kOutputAngleDesc[] = "RAV: Video FX: set output size / save angle of selected track";

// Story 10-0 -- the detection-accuracy measurement (click-based dev hook).
constexpr const char kMeasureDetectionName[] = "RAV_MEASURE_DETECTION";
constexpr const char kMeasureDetectionDesc[] = "RAV: Measure detection against reference markers";
int                  g_measure_detection_id    = 0;
gaccel_register_t    g_measure_detection_accel = {};

int                     g_command_id      = 0;
gaccel_register_t       g_accel           = {};
int                     g_add_fx_id       = 0;
gaccel_register_t       g_add_fx_accel    = {};
int                     g_pattern_id      = 0;
gaccel_register_t       g_pattern_accel   = {};
int                     g_add_shot_id     = 0;
gaccel_register_t       g_add_shot_accel  = {};
int                     g_show_shots_id   = 0;
gaccel_register_t       g_show_shots_accel = {};
int                     g_output_angle_id    = 0;
gaccel_register_t       g_output_angle_accel = {};
REAPER_PLUGIN_HINSTANCE g_hinstance       = nullptr;
HWND                    g_reaper_main     = nullptr;

// Captured at load so the rec==nullptr unload path can deregister symmetrically.
int (*g_register)(const char*, void*) = nullptr;

bool OnHookCommand(int command, int /*flag*/)
{
    if (command == 0) return false;
    if (command == g_video_bg_id) {
        ToggleVideoBackgroundOnSelectedTracks();
        RefreshToolbar(g_video_bg_id);
        return true;
    }
    if (command == g_command_id) {
        ToggleViewerWindow(g_hinstance, g_reaper_main);
        RefreshToolbar(g_command_id);  // reflect the new on/off state on the toolbar/menu now
        return true;
    }
    if (command == g_add_fx_id) {
        AddVideoFxToSelectedTracks();
        return true;
    }
    if (command == g_pattern_id) {
        SetVideoTestPatternEnabled(!VideoTestPatternEnabled());
        RefreshToolbar(g_pattern_id);
        return true;
    }
    if (command == g_add_shot_id) {
        AddVideoShotAtEditCursor();
        return true;
    }
    if (command == g_show_shots_id) {
        ShowVideoShotsOfSelectedTrack();
        return true;
    }
    if (command == g_output_angle_id) {
        SetVideoOutputAndAngleOfSelectedTrack();
        return true;
    }
    if (command == g_measure_detection_id) {
        MeasureDetectionOnSelectedItems();
        return true;
    }
    return false;
}

// Reaper polls this to draw the action's toggle checkmark / lit toolbar button.
int OnToggleAction(int command)
{
    if (command == 0) return -1;
    if (command == g_video_bg_id) return VideoBackgroundToggleState();
    if (command == g_command_id) return ViewerWindowIsVisible() ? 1 : 0;
    if (command == g_pattern_id) return VideoTestPatternEnabled() ? 1 : 0;
    return -1;  // not ours / doesn't toggle
}

// Registers one action (command id + its Actions-list entry). 0 on failure.
int RegisterAction(reaper_plugin_info_t* rec, const char* name, const char* desc, gaccel_register_t* accel)
{
    const int id = rec->Register("command_id", const_cast<char*>(name));
    if (id == 0) return 0;
    accel->accel.cmd = static_cast<WORD>(id);
    accel->desc      = desc;
    rec->Register("gaccel", accel);
    return id;
}

}  // namespace
}  // namespace rav

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(
    REAPER_PLUGIN_HINSTANCE hInstance,
    reaper_plugin_info_t*   rec)
{
    using namespace rav;

    if (!rec) {
        // Reaper is quitting. First swap in a newer Toolkit copy of our DLL, if
        // any (it is still mapped: renamed, not overwritten; see self_update.h).
        SelfUpdateOnQuit();
        // Then tear down everything we registered, in reverse order, so no
        // live pointers into our DLL outlive it. The video FX API goes first: from
        // here on a late frame request from REAPER's video thread draws nothing.
        UnregisterVideoFxApi(g_register);
        // Story 10-4b -- the marker mirror's timer.
        StopMarkerMirror();
        CloseViewerWindow();
        if (g_register) {
            // Story 11-4 -- the viewer's keyboard hook (same pointer as at load).
            g_register("-accelerator", ViewerAcceleratorRegistration());
            // Reverse-of-load teardown. -pcmsrc uses the SAME pointer the load
            // path registered, so no live pointer into our DLL survives (AC3).
            g_register("-pcmsrc",       PcmSourceRegistration());
            g_register("-toggleaction", (void*)&OnToggleAction);
            g_register("-hookcommand",  (void*)&OnHookCommand);
            if (g_measure_detection_id) g_register("-gaccel", &g_measure_detection_accel);
            if (g_video_bg_id) g_register("-gaccel", &g_video_bg_accel);
            if (g_output_angle_id) g_register("-gaccel", &g_output_angle_accel);
            if (g_show_shots_id) g_register("-gaccel", &g_show_shots_accel);
            if (g_add_shot_id)   g_register("-gaccel", &g_add_shot_accel);
            if (g_pattern_id) g_register("-gaccel", &g_pattern_accel);
            if (g_add_fx_id)  g_register("-gaccel", &g_add_fx_accel);
            g_register("-gaccel",       &g_accel);
        }
        return 0;
    }

    if (rec->caller_version != REAPER_PLUGIN_VERSION) return 0;

    if (REAPERAPI_LoadAPI(rec->GetFunc) != 0) {
        // At least one required function failed to resolve. Bail before
        // we touch a null function pointer.
        return 0;
    }

    g_hinstance   = hInstance;
    g_reaper_main = rec->hwnd_main;
    g_register    = rec->Register;

    g_command_id = RegisterAction(rec, kCommandName, kActionDesc, &g_accel);
    if (g_command_id == 0) return 0;

    // Story 11-2 -- the video FX background option. A failure only loses that action.
    g_video_bg_id = RegisterAction(rec, kVideoBgName, kVideoBgDesc, &g_video_bg_accel);

    // Story 11-1 -- video FX actions. A failure here only loses that action.
    g_add_fx_id = RegisterAction(rec, kAddVideoFxName, kAddVideoFxDesc, &g_add_fx_accel);
    g_pattern_id = RegisterAction(rec, kTestPatternName, kTestPatternDesc, &g_pattern_accel);
    // Story 11-3 -- shot test hooks.
    g_add_shot_id = RegisterAction(rec, kAddShotName, kAddShotDesc, &g_add_shot_accel);
    g_show_shots_id = RegisterAction(rec, kShowShotsName, kShowShotsDesc, &g_show_shots_accel);
    g_output_angle_id = RegisterAction(rec, kOutputAngleName, kOutputAngleDesc, &g_output_angle_accel);
    // Story 10-0 -- the detection measurement (take-marker functions resolved optionally).
    InitDetectionMeasure(rec->GetFunc);
    // Story 10-4 -- Apply's take / project marker functions (resolved optionally).
    InitTagMarkers(rec->GetFunc);
    // Story 10-4b -- RAV's project markers follow their item (a timer, independent of the window).
    StartMarkerMirror(rec->Register);
    g_measure_detection_id =
        RegisterAction(rec, kMeasureDetectionName, kMeasureDetectionDesc, &g_measure_detection_accel);

    rec->Register("hookcommand", (void*)&OnHookCommand);
    rec->Register("toggleaction", (void*)&OnToggleAction);

    // Story 11-4 -- the viewer's keys (V: RAV view / Video view, P: Video panel) and the
    // Video panel's text fields get the keyboard while the viewer has the focus.
    rec->Register("accelerator", ViewerAcceleratorRegistration());
    // Spec 11-fb-3 -- the custom keys of those (REAPER ExtState), before the viewer opens.
    LoadShortcuts();

    // Story 11-5 -- the Video panel's render buttons check REAPER's action names (optional
    // functions, resolved here so a REAPER build without them still loads the extension).
    InitReaperActionLookup(rec->GetFunc);

    // Animation files become first-class Reaper media via our PCM_source factory
    // (Story 4.1). Same pointer is handed to -pcmsrc on unload above (AC1/AC3).
    rec->Register("pcmsrc", PcmSourceRegistration());
    LogInfo("pcmsrc factory registered (.glb/.gltf/.fbx/.dae)");

    // The frame-render API rav_video_fx.clap calls (Story 11-1, src/video_fx_api.h).
    // Extensions load before any project, so it is there before the first FX instance.
    RegisterVideoFxApi(rec->Register);

    // Checks the Demute Reaper Toolkit copy on the first timer tick (ReaPack's API
    // is not loaded yet at this point). No-op for dev builds.
    SelfUpdateInit(hInstance, rec->GetFunc, rec->Register);

    LogInfo("extension loaded (Phase 0)");
    return 1;
}
