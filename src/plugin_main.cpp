// SPDX-License-Identifier: MIT
//
// Reaper extension entry point. Phase 0: registers a single action that
// opens an empty WGL viewer window. No mesh loading, no transport sync.

#include "clip_list_ui.h"  // Story 6-5-10: the clip list (its grouping in the project)
#include "console_log.h"
#include "detection_measure.h"
#include "item_rules.h"  // Story 10-6: Make tagging unique
#include "tag_markers.h"  // Story 10-4: Apply's marker functions
#include "pcm_source_anim.h"
#include "reaper_actions.h"
#include "reaper_api.h"
#include "self_update.h"
#include "sound_attach.h"
#include "sound_spread_selection.h"  // 13-68
#include "attach_manager_ui.h"       // 13-68e
#include "reaper_toast.h"            // 13-68 (P9)
#include "library_media_ui.h"
#include "library_user_ui.h"
#include "sound_audition.h"  // Epic 13 (D20): each sound module resolves its own functions
#include "sound_follow.h"
#include "sound_peaks.h"
#include "sound_place.h"
#include "sound_populate.h"
#include "sound_project.h"
#include "video_fx_host.h"
#include "video_fx_track.h"
#include "video_timeline.h"
#include "video_shots.h"
#include "shortcuts.h"
#include "viewer_window.h"
#include "watchdog.h"  // freeze audit: breadcrumbs, heartbeats, the freeze report

#include "rav_version.h"

#include <cstring>

namespace rav {
void LoadZoneNewSnapMode();  // zone_wave_ui.cpp (13-67b; its header pulls ImGui)
}  // namespace rav

namespace rav {
namespace {

constexpr const char kCommandName[] = "RAV_OPEN_VIEWER";
constexpr const char kActionDesc[]  = "RAV: Viewer - Open viewer";

// Story 6-5-9 -- the viewer full screen (a toggle: lit while full screen).
constexpr const char kFullScreenName[] = "RAV_VIEWER_FULLSCREEN";
constexpr const char kFullScreenDesc[] = "RAV: Viewer - Full screen (toggle)";
int                  g_full_screen_id    = 0;
gaccel_register_t    g_full_screen_accel = {};

// Story 11-1 -- the video FX actions.
constexpr const char kAddVideoFxName[]     = "RAV_VIDEOFX_ADD";
constexpr const char kAddVideoFxDesc[]     = "RAV: Video FX - Add to selected track";
constexpr const char kTestPatternName[]    = "RAV_VIDEOFX_TEST_PATTERN";
constexpr const char kTestPatternDesc[]    = "RAV: Video FX - Test pattern";

// Story 11-2 -- the video FX background option (viewer background / transparent).
constexpr const char kVideoBgName[] = "RAV_VIDEOFX_TRANSPARENT_BG";
constexpr const char kVideoBgDesc[] = "RAV: Video FX - Transparent background (selected tracks)";
int                  g_video_bg_id    = 0;
gaccel_register_t    g_video_bg_accel = {};

// Story 11-3 -- click-based test hooks for the shots (11-4/11-5 call the same helpers).
constexpr const char kAddShotName[]  = "RAV_VIDEOFX_ADD_SHOT";
constexpr const char kAddShotDesc[]  = "RAV: Video FX - Add shot at edit cursor";
constexpr const char kShowShotsName[] = "RAV_VIDEOFX_SHOW_SHOTS";
constexpr const char kShowShotsDesc[] = "RAV: Video FX - Show shots of selected track";
constexpr const char kOutputAngleName[] = "RAV_VIDEOFX_SET_OUTPUT_ANGLE";
constexpr const char kOutputAngleDesc[] = "RAV: Video FX - Set output size / save angle of selected track";

// Story 10-0 -- the detection-accuracy measurement (click-based dev hook).
constexpr const char kMeasureDetectionName[] = "RAV_MEASURE_DETECTION";
constexpr const char kMeasureDetectionDesc[] = "RAV: Dev - Measure detection against reference markers";
int                  g_measure_detection_id    = 0;
gaccel_register_t    g_measure_detection_accel = {};

// Story 10-6 -- pooled copies: the selected items stop sharing their tagging with the other
// items of their animation file (click-based hook too).
constexpr const char kMakeUniqueName[] = "RAV_TAGGING_MAKE_UNIQUE";
constexpr const char kMakeUniqueDesc[] = "RAV: Tagging - Make tagging unique (selected items)";
int                  g_make_unique_id    = 0;
gaccel_register_t    g_make_unique_accel = {};

// Epic 13 (D20, D10) -- the sound library storage check (a dev check, click-based).
constexpr const char kSoundStoreCheckName[] = "RAV_SOUND_STORE_CHECK";
constexpr const char kSoundStoreCheckDesc[] = "RAV: Dev - Sound library storage check";
int                  g_sound_check_id    = 0;
gaccel_register_t    g_sound_check_accel = {};

// Lot 13-60b -- one action per viewer key (src/shortcuts.h ShortcutDef::action): a Shortcuts row on
// "REAPER" runs on the keys the user gives it in REAPER's Actions window. Full screen has its own above.
// Story 13-68 -- spread the selected sounds onto every marker of the selected marker's type.
constexpr const char kSpreadThisName[] = "RAV_SPREAD_SELECTION_THIS_ANIMATION";
constexpr const char kSpreadThisDesc[] = "RAV: Tagging - Attach selected items to selected marker (this animation)";
constexpr const char kSpreadAllName[]  = "RAV_SPREAD_SELECTION_ALL_INSTANCES";
constexpr const char kSpreadAllDesc[]  = "RAV: Tagging - Attach selected items to selected marker (all animations with this marker)";
int                  g_spread_this_id    = 0;
gaccel_register_t    g_spread_this_accel = {};
int                  g_spread_all_id     = 0;
gaccel_register_t    g_spread_all_accel  = {};
constexpr const char kStackThisName[] = "RAV_ADD_SELECTION_TO_STACK_THIS_ANIMATION";
constexpr const char kStackThisDesc[] = "RAV: Tagging - Add selected items to stack (this animation)";
int                  g_stack_this_id    = 0;
gaccel_register_t    g_stack_this_accel = {};
// 13-68 v3: the all-animations Add to stack again, on its old id (old bindings keep working).
constexpr const char kStackAllName[]  = "RAV_ADD_SELECTION_TO_STACK_ALL_INSTANCES";
constexpr const char kStackAllDesc[]  = "RAV: Tagging - Add selected items to stack (all animations with this marker)";
int                  g_stack_all_id     = 0;
gaccel_register_t    g_stack_all_accel  = {};
// 13-68e -- the Attach manager, opened at the mouse (v5: one window, no Detach tab; the id of v1's Attach tab action
// kept, so its bindings still open it).
constexpr const char kAttachMgrName[]  = "RAV_ATTACH_MANAGER_ATTACH";
constexpr const char kAttachMgrDesc[]  = "RAV: Attach manager (at mouse)";
int                  g_attach_mgr_id     = 0;
gaccel_register_t    g_attach_mgr_accel  = {};

int                     g_key_ids[kShortcutCount]    = {};
gaccel_register_t       g_key_accels[kShortcutCount] = {};

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

// Freeze audit: one of RAV's actions (a crumb and a scope while it runs; REAPER's own are not noted).
bool OurCommand(int c)
{
    for (int i = 0; i < kShortcutCount; ++i)
        if (g_key_ids[i] != 0 && c == g_key_ids[i]) return true;
    for (int id : {g_video_bg_id, g_command_id, g_full_screen_id, g_add_fx_id, g_pattern_id, g_add_shot_id, g_show_shots_id,
                   g_output_angle_id, g_measure_detection_id, g_make_unique_id, g_spread_this_id, g_spread_all_id, g_stack_this_id, g_stack_all_id,
                   g_sound_check_id, g_attach_mgr_id})
        if (id != 0 && c == id) return true;
    return false;
}

bool OnHookCommandBody(int command);

bool OnHookCommand(int command, int /*flag*/)
{
    if (command == 0 || !OurCommand(command)) return false;
    RAV_SCOPE("Command", command);
    return OnHookCommandBody(command);
}

// Freeze audit: the main thread's heartbeat (REAPER's timer, ~30 Hz, also while no RAV window is open), and the
// last session's freeze report pointed to once, a few seconds after the load.
void WatchdogTimer()
{
    wd::MainBeat();
    static int ticks = 0;
    if (ticks < 1000) ++ticks;
    if (ticks == 90 && wd::TakeFreezeToast())
        ShowReaperToast("RAV saved a freeze report from the last session (REAPER had stopped answering):\n" + wd::FreezeReportPath() +
                        "\nPlease send that file (Copy error log / Copy details also starts with it).");
}

bool OnHookCommandBody(int command)
{
    for (int i = 0; i < kShortcutCount; ++i)  // 13-60b: a viewer key's action
        if (g_key_ids[i] != 0 && command == g_key_ids[i]) {
            RunViewerShortcut(i);
            return true;
        }
    if (command == g_video_bg_id) {
        ToggleVideoBackgroundOnSelectedTracks();
        RefreshToolbar(g_video_bg_id);
        return true;
    }
    if (command == g_command_id) {
        ToggleViewerWindow(g_hinstance, g_reaper_main);
        RefreshToolbar(g_command_id);  // reflect the new on/off state on the toolbar/menu now
        if (g_full_screen_id) RefreshToolbar(g_full_screen_id);
        return true;
    }
    if (command == g_full_screen_id) {
        ToggleViewerFullScreen(g_hinstance, g_reaper_main);
        RefreshToolbar(g_full_screen_id);
        RefreshToolbar(g_command_id);  // it may have opened the viewer
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
    if (command == g_make_unique_id) {
        MakeTaggingUniqueOnSelectedItems();
        return true;
    }
    if (command == g_spread_this_id) {
        SpreadSelectionToMarkerType(SpreadScope::ThisAnimation);
        return true;
    }
    if (command == g_spread_all_id) {
        SpreadSelectionToMarkerType(SpreadScope::AllInstances);
        return true;
    }
    if (command == g_stack_this_id) {
        AddSelectionToStack(SpreadScope::ThisAnimation);
        return true;
    }
    if (command == g_stack_all_id) {
        AddSelectionToStack(SpreadScope::AllInstances);
        return true;
    }
    if (command == g_sound_check_id) {
        RunSoundStoreCheck();
        return true;
    }
    if (command == g_attach_mgr_id) {  // 13-68e
        // The window draws through the viewer's GL context: the viewer is made when it does not exist yet.
        if (!ViewerWindowExists()) OpenViewerWindow(g_hinstance, g_reaper_main);
        OpenAttachManager();
        RefreshToolbar(g_command_id);
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
    if (command == g_full_screen_id) return ViewerIsFullScreen() ? 1 : 0;
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
        // Freeze audit: the watchdog stops first (the unload below may take long: not a freeze), its timer goes.
        wd::StopWatchdog();
        if (g_register) g_register("-timer", reinterpret_cast<void*>(&WatchdogTimer));
        // Reaper is quitting. First swap in a newer Toolkit copy of our DLL, if
        // any (it is still mapped: renamed, not overwritten; see self_update.h).
        SelfUpdateOnQuit();
        // Then tear down everything we registered, in reverse order, so no
        // live pointers into our DLL outlive it. The video FX API goes first: from
        // here on a late frame request from REAPER's video thread draws nothing.
        UnregisterVideoFxApi(g_register);
        // Story 10-4b -- the marker mirror's timer.
        StopMarkerMirror();
        StopAudition();  // Story 13-7b -- a playing audition is stopped and freed before unload
        CloseViewerWindow();
        // Epic 13 (D20) -- the sound store's projectconfig.
        QuitSoundProject();
        // Story 6-5-10 -- the clip list's projectconfig line (its grouping).
        QuitClipList();
        // 13-7c -- stops the media preview, joins the peak reader and the database load.
        QuitLibraryMedia();
        // 13-55 -- the one peak reader's thread (Sound panel, Edit, Media Explorer).
        QuitSoundPeaks();
        // 13-11 -- the User Library timer and the Locate worker.
        QuitLibraryUser();
        QuitReaperToast();  // 13-68
        if (g_register) {
            // Story 11-4 -- the viewer's keyboard hook (same pointer as at load).
            g_register("-accelerator", ViewerAcceleratorRegistration());
            // Reverse-of-load teardown. -pcmsrc uses the SAME pointer the load
            // path registered, so no live pointer into our DLL survives (AC3).
            g_register("-pcmsrc",       PcmSourceRegistration());
            g_register("-toggleaction", (void*)&OnToggleAction);
            g_register("-hookcommand",  (void*)&OnHookCommand);
            if (g_full_screen_id) g_register("-gaccel", &g_full_screen_accel);
            for (int i = 0; i < kShortcutCount; ++i)  // 13-60b
                if (g_key_ids[i]) g_register("-gaccel", &g_key_accels[i]);
            if (g_sound_check_id) g_register("-gaccel", &g_sound_check_accel);
            if (g_spread_this_id) g_register("-gaccel", &g_spread_this_accel);
            if (g_spread_all_id) g_register("-gaccel", &g_spread_all_accel);
            if (g_stack_this_id) g_register("-gaccel", &g_stack_this_accel);
            if (g_stack_all_id) g_register("-gaccel", &g_stack_all_accel);
            if (g_attach_mgr_id) g_register("-gaccel", &g_attach_mgr_accel);  // 13-68e
            g_register("-hookpostcommand", (void*)&AttachWatchPostCommand);  // 13-68e v5
            g_register("-timer", reinterpret_cast<void*>(&AttachWatchTick));  // 13-68e v5: the linked instances' watcher
            if (g_make_unique_id) g_register("-gaccel", &g_make_unique_accel);
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

    // Freeze audit: the watchdog (its own thread; it never calls the REAPER API: the resource path is read here),
    // started once the load can no longer fail. Armed by the first heartbeat of its timer: REAPER's own load is
    // never taken for a freeze.
    wd::StartWatchdog(GetResourcePath(), RAV_DISPLAY_VERSION);
    rec->Register("timer", reinterpret_cast<void*>(&WatchdogTimer));

    // Story 6-5-9 -- the viewer full screen. A failure only loses that action (F11 and the
    // viewer's button still work).
    g_full_screen_id = RegisterAction(rec, kFullScreenName, kFullScreenDesc, &g_full_screen_accel);
    SetViewerFullScreenCommandId(g_full_screen_id);
    // Lot 13-60b -- the viewer keys' actions (no default key: the user gives them one in REAPER). A
    // failure only loses that action (its row's "REAPER" then finds no action in REAPER's list).
    for (int i = 0; i < kShortcutCount; ++i)  // 13-61: a row on one of REAPER's own actions has none
        if (kShortcutTable[i].action && std::strcmp(kShortcutTable[i].action, kFullScreenName) != 0)
            g_key_ids[i] = RegisterAction(rec, kShortcutTable[i].action, kShortcutTable[i].action_desc, &g_key_accels[i]);

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
    // Story 10-6 -- Make tagging unique (selected items). A failure only loses that action.
    g_make_unique_id = RegisterAction(rec, kMakeUniqueName, kMakeUniqueDesc, &g_make_unique_accel);
    // Epic 13 (D20) -- the sound store (projectconfig), each sound module's own functions (D7), and
    // the storage check action (D10). A failure only loses that part.
    InitSoundProject(rec);
    // Story 6-5-10 -- the clip list: its grouping in the project, REAPER's item selection functions.
    InitClipList(rec);
    InitSoundPlace(rec->GetFunc);
    InitSoundPopulate(rec->GetFunc);
    InitSoundPeaks(rec->GetFunc);
    InitSoundAudition(rec->GetFunc);
    InitSoundFollow(rec->GetFunc);
    InitSoundAttach(rec->GetFunc);
    InitLibraryMedia(rec->GetFunc);  // 13-7c: preview + the media database load (on first draw)
    InitLibraryUser(rec);            // 13-11: User Libraries (.ravlib), File Sound import, Locate
    g_sound_check_id = RegisterAction(rec, kSoundStoreCheckName, kSoundStoreCheckDesc, &g_sound_check_accel);
    InitReaperToast(hInstance, rec->hwnd_main);  // 13-68 (P9)
    InitSoundSpreadSelection(rec->GetFunc);
    g_spread_this_id = RegisterAction(rec, kSpreadThisName, kSpreadThisDesc, &g_spread_this_accel);
    g_spread_all_id = RegisterAction(rec, kSpreadAllName, kSpreadAllDesc, &g_spread_all_accel);
    g_stack_this_id = RegisterAction(rec, kStackThisName, kStackThisDesc, &g_stack_this_accel);
    g_stack_all_id = RegisterAction(rec, kStackAllName, kStackAllDesc, &g_stack_all_accel);
    g_attach_mgr_id = RegisterAction(rec, kAttachMgrName, kAttachMgrDesc, &g_attach_mgr_accel);  // 13-68e
    // 13-68e v5: a gesture on one instance in REAPER reaches its linked instances (also with the window closed).
    rec->Register("timer", reinterpret_cast<void*>(&AttachWatchTick));
    rec->Register("hookpostcommand", (void*)&AttachWatchPostCommand);

    rec->Register("hookcommand", (void*)&OnHookCommand);
    rec->Register("toggleaction", (void*)&OnToggleAction);

    // Story 11-4 -- the viewer's keys (V: RAV view / Video view, P: Video panel) and the
    // Video panel's text fields get the keyboard while the viewer has the focus.
    rec->Register("accelerator", ViewerAcceleratorRegistration());
    // Spec 11-fb-3 -- the custom keys of those (REAPER ExtState), before the viewer opens.
    LoadShortcuts();
    LoadZoneNewSnapMode();  // 13-67b: the snap offset mode new Sounds get (Sound Edit Options), before any is made

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
