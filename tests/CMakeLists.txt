# Host tests of RAV's pure, REAPER-free helpers. A project of its own (the main build
# needs Windows and REAPER); any platform, any C++17 compiler:
#   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
cmake_minimum_required(VERSION 3.20)
project(rav_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# The REAPER-only parts of shared sources stay out (they test _WIN32, which MSVC also defines here).
add_compile_definitions(RAV_HOST_TEST)
# Story 10-9-0: the auto detection and its types' table (src/auto_types*.cpp), linked together.
set(RAV_AUTO_SRCS ../src/auto_detect.cpp ../src/auto_types.cpp ../src/auto_types_legs.cpp ../src/auto_types_arms.cpp
                  ../src/auto_types_jolt.cpp)

enable_testing()

add_executable(video_shot_timing_test video_shot_timing_test.cpp)
target_include_directories(video_shot_timing_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME video_shot_timing COMMAND video_shot_timing_test)

add_executable(video_preview_lag_test video_preview_lag_test.cpp ../src/video_preview_lag.cpp)
target_include_directories(video_preview_lag_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
find_package(Threads REQUIRED)
target_link_libraries(video_preview_lag_test PRIVATE Threads::Threads)
add_test(NAME video_preview_lag COMMAND video_preview_lag_test)

# Spec 11-fb-3 -- the viewer's shortcut rules (src/shortcuts.h).
add_executable(shortcuts_test shortcuts_test.cpp)
target_include_directories(shortcuts_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME shortcuts COMMAND shortcuts_test)

# Story 6-5-9 -- the viewer's full screen plan (src/viewer_fullscreen_plan.h).
add_executable(viewer_fullscreen_plan_test viewer_fullscreen_plan_test.cpp)
target_include_directories(viewer_fullscreen_plan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME viewer_fullscreen_plan COMMAND viewer_fullscreen_plan_test)

# 13-7a / 13-10 -- a gesture's Undo only while it is REAPER's last undo point (src/undo_guard.h).
add_executable(undo_guard_test undo_guard_test.cpp)
target_include_directories(undo_guard_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME undo_guard COMMAND undo_guard_test)

# 13-8 fix -- a menu's gesture runs after the widgets, with its own copies (src/picked_gestures.h).
add_executable(picked_gestures_test picked_gestures_test.cpp)
target_include_directories(picked_gestures_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME picked_gestures COMMAND picked_gestures_test)
# 13-53 -- a dragged value released stays drawn until the store holds it (src/live_draft_phase.h).
add_executable(live_draft_phase_test live_draft_phase_test.cpp)
target_include_directories(live_draft_phase_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME live_draft_phase COMMAND live_draft_phase_test)

# 13-74 -- the selected zones' gain a notch at a time (src/zone_gain_nudge.h).
add_executable(zone_gain_nudge_test zone_gain_nudge_test.cpp)
target_include_directories(zone_gain_nudge_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME zone_gain_nudge COMMAND zone_gain_nudge_test)
# Freeze audit -- the watchdog's breadcrumb ring, scope stack and report text (src/watchdog.cpp).
add_executable(watchdog_test watchdog_test.cpp ../src/watchdog.cpp)
target_include_directories(watchdog_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(watchdog_test PRIVATE RAV_WATCHDOG_HOST)
target_link_libraries(watchdog_test PRIVATE Threads::Threads)
add_test(NAME watchdog COMMAND watchdog_test)
# Freeze audit #8 -- a join that gives up (src/timed_join.h).
add_executable(timed_join_test timed_join_test.cpp ../src/watchdog.cpp)
target_include_directories(timed_join_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(timed_join_test PRIVATE RAV_WATCHDOG_HOST)
target_link_libraries(timed_join_test PRIVATE Threads::Threads)
add_test(NAME timed_join COMMAND timed_join_test)
# Freeze audit #3 -- a replaced store is retired, freed only at a top-level point (src/store_graveyard.h).
add_executable(store_graveyard_test store_graveyard_test.cpp)
target_include_directories(store_graveyard_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME store_graveyard COMMAND store_graveyard_test)
# Freeze audit #1 -- dashes and hatches walk only their visible marks, capped (src/dash_plan.h).
add_executable(dash_plan_test dash_plan_test.cpp)
target_include_directories(dash_plan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME dash_plan COMMAND dash_plan_test)
# The same rule in the Library view: its lists, menus, rename fields and drops pick their
# gestures over a real sound store (src/library_tree_edit.h), run after the loop.
add_executable(library_picked_gestures_test library_picked_gestures_test.cpp ../src/library_tree_edit.cpp ../src/sound_switch.cpp ../src/sound_entries.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp)
target_include_directories(library_picked_gestures_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME library_picked_gestures COMMAND library_picked_gestures_test)

# Spec 11-fb-13 -- the Output size written into REAPER's render format (src/render_format_size.h).
add_executable(render_format_size_test render_format_size_test.cpp)
target_include_directories(render_format_size_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME render_format_size COMMAND render_format_size_test)

# Spec 11-fb-15 -- shots belong to clips (src/video_clip_shots.h).
add_executable(video_clip_shots_test video_clip_shots_test.cpp)
target_include_directories(video_clip_shots_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME video_clip_shots COMMAND video_clip_shots_test)

# Spec 11-fb-16 -- Render Current's source switch (src/render_current.h).
add_executable(render_current_test render_current_test.cpp)
target_include_directories(render_current_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME render_current COMMAND render_current_test)

# Spec 10-3c -- the skeleton overlay's projection, pick and segments (src/skeleton_overlay.h).
add_executable(skeleton_overlay_test skeleton_overlay_test.cpp)
target_include_directories(skeleton_overlay_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME skeleton_overlay COMMAND skeleton_overlay_test)

# Issue #1 -- "Locate textures...": file-name extraction + search/copy (src/texture_locate.h).
add_executable(texture_locate_test texture_locate_test.cpp ../src/texture_locate.cpp)
target_include_directories(texture_locate_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME texture_locate COMMAND texture_locate_test)

# Spec 10-0 -- the bone-event rule engine and role mapping (src/bone_events.h, bone_roles.h).
add_executable(bone_events_test bone_events_test.cpp ../src/bone_events.cpp ../src/footstep_measure.cpp)
target_include_directories(bone_events_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME bone_events COMMAND bone_events_test)

# Spike 10-8a -- foot events from body physics (src/motion_physics.h).
add_executable(motion_physics_test motion_physics_test.cpp ../src/motion_physics.cpp ../src/bone_events.cpp)
target_include_directories(motion_physics_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME motion_physics COMMAND motion_physics_test)

# Story 10-8b -- auto detection: foot events from the physics as blocks of the item (src/auto_detect.h).
add_executable(auto_detect_test auto_detect_test.cpp ${RAV_AUTO_SRCS} ../src/motion_physics.cpp ../src/bone_events.cpp
               ../src/rule_record.cpp ../src/event_list.cpp ../src/tagging_signal.cpp)
target_include_directories(auto_detect_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME auto_detect COMMAND auto_detect_test)

# Story 10-9-0 -- the auto event types' table (src/auto_types.h).
add_executable(auto_types_test auto_types_test.cpp ${RAV_AUTO_SRCS} ../src/motion_physics.cpp ../src/bone_events.cpp ../src/rule_record.cpp ../src/event_list.cpp
               ../src/tagging_signal.cpp ../src/sound_assign.cpp ../src/sound_auto_assign.cpp ../src/sound_populate_plan.cpp
               ../src/sound_switch.cpp ../src/sound_draw.cpp ../src/sound_place_plan.cpp ../src/sound_entries.cpp
               ../src/sound_record.cpp ../src/sound_zones.cpp)
target_include_directories(auto_types_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME auto_types COMMAND auto_types_test)

add_executable(bone_roles_test bone_roles_test.cpp ../src/bone_events.cpp)
target_include_directories(bone_roles_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME bone_roles COMMAND bone_roles_test)

# Spec 10-7a -- "REF <label>" reference markers and their CSV lines (src/ref_labels.h).
add_executable(ref_labels_test ref_labels_test.cpp ../src/bone_events.cpp ../src/role_map_store.cpp
               ../src/rule_record.cpp)
target_include_directories(ref_labels_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME ref_labels COMMAND ref_labels_test)

# Story 10-8n -- detection_eval's BIAS block pieces (tests/event_bias.h).
add_executable(event_bias_test event_bias_test.cpp)
target_include_directories(event_bias_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME event_bias COMMAND event_bias_test)

# Spec 10-0 -- the offline evaluator: runs the footstep detection on RAV_detection_dump CSV
# files and prints the measure action's report (a tool, not a test):
#   detection_eval [--param=value ...] [--json] file.csv...
# Spike 10-7a step 1: --single measures Speed, Stillness and Relative drop alone per labelled bone.
# Spike 10-8a: --physics scores the foot events of motion_physics.h against the labelled REFs.
# Deferred 10-9a-2: the jump / land REFs are scored on the Detect path's merged signatures (auto sources).
add_executable(detection_eval detection_eval.cpp ${RAV_AUTO_SRCS} ../src/bone_events.cpp ../src/footstep_measure.cpp
               ../src/tagging_signal.cpp ../src/motion_physics.cpp ../src/rule_record.cpp ../src/event_list.cpp)
target_include_directories(detection_eval PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# Story 10-8o -- the all-trigger bench (a tool, not a test): every auto type ticked, through Detect
# (DetectAutoEvents), on every fixture, scored against the reference files (tests/ref_file.h):
#   trigger_bench [--fixtures=<dir>] [--fbx-dump=<exe>] [--out=<dir>]   (tools/bench/run.sh runs it)
add_executable(trigger_bench trigger_bench.cpp ${RAV_AUTO_SRCS} ../src/motion_physics.cpp ../src/bone_events.cpp
               ../src/rule_record.cpp ../src/event_list.cpp ../src/tagging_signal.cpp)
target_include_directories(trigger_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# Story 10-8o -- the bench's reference file and row match (tests/ref_file.h).
add_executable(ref_file_test ref_file_test.cpp)
target_include_directories(ref_file_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME ref_file COMMAND ref_file_test)

# Story 10-2 -- the rules record, presets and the role mapping (src/rule_record.h,
# preset_store.h, role_map_store.h). The factory presets are embedded as in the DLL.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(FactoryPresets)
rav_generate_factory_presets("${CMAKE_CURRENT_SOURCE_DIR}/../presets/factory"
                             "${CMAKE_CURRENT_SOURCE_DIR}/../src/factory_presets.h.in"
                             "${CMAKE_CURRENT_BINARY_DIR}/generated/factory_presets.h")

add_executable(rule_record_test rule_record_test.cpp ../src/rule_record.cpp)
target_include_directories(rule_record_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(rule_record_test PRIVATE RAV_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME rule_record COMMAND rule_record_test)

add_executable(preset_store_test preset_store_test.cpp ../src/rule_record.cpp ../src/preset_store.cpp)
target_include_directories(preset_store_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src
                           ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(preset_store_test PRIVATE RAV_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME preset_store COMMAND preset_store_test)

add_executable(role_map_store_test role_map_store_test.cpp ../src/role_map_store.cpp ../src/rule_record.cpp)
target_include_directories(role_map_store_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(role_map_store_test PRIVATE RAV_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME role_map_store COMMAND role_map_store_test)

# Story 10-3 -- the Tagging view's units, menus and clip mapping (src/tagging_signal.h).
add_executable(tagging_signal_test tagging_signal_test.cpp ../src/tagging_signal.cpp)
target_include_directories(tagging_signal_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME tagging_signal COMMAND tagging_signal_test)

# Story 10-4 -- the item's event list, Apply's plan, the first pass (src/event_list.h).
# fb-1 (10-8b): the keep step lives in auto_detect (KeepUnrunnableEvents), with the physics it needs.
add_executable(event_list_test event_list_test.cpp ../src/event_list.cpp ../src/rule_record.cpp ../src/tagging_signal.cpp
               ${RAV_AUTO_SRCS} ../src/motion_physics.cpp ../src/bone_events.cpp)
target_include_directories(event_list_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME event_list COMMAND event_list_test)

# Story 10-3b -- the Tagging view's preset menu logic (src/preset_menu_model.h).
add_executable(preset_menu_model_test preset_menu_model_test.cpp ../src/preset_menu_model.cpp)
target_include_directories(preset_menu_model_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME preset_menu_model COMMAND preset_menu_model_test)

# Epic 13 (D20, story 13-2) -- the sound model's text (src/sound_record.h).
add_executable(sound_record_test sound_record_test.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/sound_entries.cpp ../src/rule_record.cpp)
target_include_directories(sound_record_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME sound_record COMMAND sound_record_test)
# 13-35 -- Sync placed sounds: prints, behind, hand edits, the Library's view (src/sound_sync_plan.h).
add_executable(sound_sync_plan_test sound_sync_plan_test.cpp ../src/sound_sync_plan.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/sound_entries.cpp ../src/rule_record.cpp)
target_include_directories(sound_sync_plan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME sound_sync_plan COMMAND sound_sync_plan_test)

# Epic 13 -- one target per pure module (13-2 placeholders; each wave story replaces its own test).
# Story 13-3 -- src/sound_entries.h
add_executable(sound_entries_test sound_entries_test.cpp ../src/sound_entries.cpp ../src/sound_draw.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp)
target_include_directories(sound_entries_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME sound_entries COMMAND sound_entries_test)

# Story 13-14 -- the peak lane helpers of src/sound_model.h (stereo waveform lanes).
add_executable(peak_lanes_test peak_lanes_test.cpp)
target_include_directories(peak_lanes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME peak_lanes COMMAND peak_lanes_test)
# Story 13-3 -- src/sound_draw.h
add_executable(sound_draw_test sound_draw_test.cpp ../src/sound_draw.cpp ../src/sound_entries.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp)
target_include_directories(sound_draw_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME sound_draw COMMAND sound_draw_test)
# Story 13-4 -- src/sound_switch.h
add_executable(sound_switch_test sound_switch_test.cpp ../src/sound_switch.cpp ../src/sound_entries.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp)
target_include_directories(sound_switch_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME sound_switch COMMAND sound_switch_test)
# Story 13-7a -- src/library_tree_edit.h
add_executable(library_tree_edit_test library_tree_edit_test.cpp ../src/library_tree_edit.cpp ../src/sound_switch.cpp ../src/sound_entries.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp)
target_include_directories(library_tree_edit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME library_tree_edit COMMAND library_tree_edit_test)
# Story 13-7c -- src/library_media_plan.h
add_executable(library_media_plan_test library_media_plan_test.cpp ../src/library_media_plan.cpp ../src/library_tree_edit.cpp ../src/sound_switch.cpp ../src/sound_entries.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp)
target_include_directories(library_media_plan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME library_media_plan COMMAND library_media_plan_test)
# Story 13-5 -- src/media_db.h
add_executable(media_db_test media_db_test.cpp ../src/media_db.cpp)
target_include_directories(media_db_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(media_db_test PRIVATE Threads::Threads)
target_compile_definitions(media_db_test PRIVATE RAV_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME media_db COMMAND media_db_test)
# Story 13-6 -- src/sound_place_plan.h
add_executable(sound_place_plan_test sound_place_plan_test.cpp ../src/sound_place_plan.cpp ../src/sound_entries.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp)
target_include_directories(sound_place_plan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME sound_place_plan COMMAND sound_place_plan_test)
# Story 13-10 -- src/sound_attach_plan.h (13-68: marker types; the toast's host stub, never a window)
add_executable(sound_attach_plan_test sound_attach_plan_test.cpp ../src/sound_attach_plan.cpp ../src/reaper_toast.cpp ../src/sound_draw.cpp ../src/sound_entries.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp)
target_include_directories(sound_attach_plan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME sound_attach_plan COMMAND sound_attach_plan_test)
# Story 13-68 v2 -- src/sound_spread_plan.h (the selection as an Attached library)
add_executable(sound_spread_plan_test sound_spread_plan_test.cpp ../src/sound_spread_plan.cpp ../src/sound_attach_plan.cpp ../src/sound_draw.cpp ../src/sound_entries.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp)
target_include_directories(sound_spread_plan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME sound_spread_plan COMMAND sound_spread_plan_test)
# 13-68e -- the Attach manager's lists, choices and record edits (src/attach_manager_plan.h), RAV - Side (sound_spread_plan.h)
add_executable(attach_manager_plan_test attach_manager_plan_test.cpp ../src/attach_manager_plan.cpp ../src/sound_spread_plan.cpp ../src/sound_attach_plan.cpp ../src/sound_draw.cpp ../src/sound_entries.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp)
target_include_directories(attach_manager_plan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME attach_manager_plan COMMAND attach_manager_plan_test)
# Story 13-8 -- src/sound_populate_plan.h
add_executable(sound_populate_plan_test sound_populate_plan_test.cpp ../src/sound_populate_plan.cpp ../src/sound_switch.cpp
               ../src/sound_draw.cpp ../src/sound_place_plan.cpp ../src/sound_entries.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp
               ../src/sound_assign.cpp ../src/sound_auto_assign.cpp ${RAV_AUTO_SRCS} ../src/motion_physics.cpp
               ../src/bone_events.cpp ../src/event_list.cpp ../src/tagging_signal.cpp)
target_include_directories(sound_populate_plan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME sound_populate_plan COMMAND sound_populate_plan_test)
# Story 13-8, Antho 2026-10-12 -- src/sound_auto_assign.h (each auto marker type's sound)
add_executable(sound_auto_assign_test sound_auto_assign_test.cpp ../src/sound_auto_assign.cpp ../src/sound_assign.cpp ../src/sound_populate_plan.cpp
               ../src/sound_switch.cpp ../src/sound_draw.cpp ../src/sound_place_plan.cpp ../src/sound_entries.cpp
               ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp ${RAV_AUTO_SRCS} ../src/motion_physics.cpp
               ../src/bone_events.cpp ../src/event_list.cpp ../src/tagging_signal.cpp)
target_include_directories(sound_auto_assign_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME sound_auto_assign COMMAND sound_auto_assign_test)
# Story 13-15a -- src/sound_assign.h (layered sound assignment) and its text
add_executable(sound_assign_test sound_assign_test.cpp ../src/sound_assign.cpp ../src/sound_auto_assign.cpp
               ../src/sound_populate_plan.cpp ../src/sound_switch.cpp ../src/sound_draw.cpp ../src/sound_place_plan.cpp
               ../src/sound_entries.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp ${RAV_AUTO_SRCS}
               ../src/motion_physics.cpp ../src/bone_events.cpp ../src/event_list.cpp ../src/tagging_signal.cpp)
target_include_directories(sound_assign_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME sound_assign COMMAND sound_assign_test)
# Story 13-9 -- src/sound_follow_plan.h
add_executable(sound_follow_plan_test sound_follow_plan_test.cpp ../src/sound_follow_plan.cpp ../src/reaper_item_read.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp ../src/sound_draw.cpp ../src/sound_entries.cpp ../src/event_list.cpp ../src/tagging_signal.cpp ${RAV_AUTO_SRCS} ../src/motion_physics.cpp ../src/bone_events.cpp)
target_include_directories(sound_follow_plan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME sound_follow_plan COMMAND sound_follow_plan_test)
# Story 13-11 -- src/ravlib.h
add_executable(ravlib_test ravlib_test.cpp ../src/ravlib.cpp ../src/sound_entries.cpp ../src/sound_place_plan.cpp ../src/library_tree_edit.cpp ../src/sound_switch.cpp ../src/media_db.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp)
target_link_libraries(ravlib_test PRIVATE Threads::Threads)
target_include_directories(ravlib_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME ravlib COMMAND ravlib_test)
# 13-77b -- the three-way library <-> file sync engine (src/library_sync.h).
add_executable(library_sync_test library_sync_test.cpp ../src/library_sync.cpp ../src/ravlib.cpp ../src/sound_entries.cpp ../src/sound_place_plan.cpp ../src/library_tree_edit.cpp ../src/sound_switch.cpp ../src/media_db.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp)
target_link_libraries(library_sync_test PRIVATE Threads::Threads)
target_include_directories(library_sync_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME library_sync COMMAND library_sync_test)
# 13-77c -- the Sync with file window's selection model (src/library_sync_select.h).
add_executable(library_sync_select_test library_sync_select_test.cpp ../src/library_sync_select.cpp ../src/library_sync.cpp ../src/ravlib.cpp ../src/sound_entries.cpp ../src/sound_place_plan.cpp ../src/library_tree_edit.cpp ../src/sound_switch.cpp ../src/media_db.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp)
target_link_libraries(library_sync_select_test PRIVATE Threads::Threads)
target_include_directories(library_sync_select_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME library_sync_select COMMAND library_sync_select_test)
# 13-77c readability pass -- the window's row grouping and display names (src/sync_rows.h).
add_executable(sync_rows_test sync_rows_test.cpp ../src/sync_rows.cpp ../src/library_sync_select.cpp ../src/library_sync.cpp ../src/ravlib.cpp ../src/sound_entries.cpp ../src/sound_place_plan.cpp ../src/library_tree_edit.cpp ../src/sound_switch.cpp ../src/media_db.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp)
target_link_libraries(sync_rows_test PRIVATE Threads::Threads)
target_include_directories(sync_rows_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME sync_rows COMMAND sync_rows_test)
# Story 13-13 -- src/sound_tracks_plan.h (Switch-named tracks)
add_executable(sound_tracks_plan_test sound_tracks_plan_test.cpp ../src/sound_tracks_plan.cpp ../src/ravlib.cpp ../src/sound_entries.cpp ../src/library_tree_edit.cpp ../src/sound_switch.cpp ../src/media_db.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp)
target_link_libraries(sound_tracks_plan_test PRIVATE Threads::Threads)
target_include_directories(sound_tracks_plan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME sound_tracks_plan COMMAND sound_tracks_plan_test)
# Story 13-28 -- src/normalize_presets.h (the user's normalize presets, pure)
add_executable(normalize_presets_test normalize_presets_test.cpp ../src/normalize_presets.cpp)
target_include_directories(normalize_presets_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME normalize_presets COMMAND normalize_presets_test)

# Story 13-7b -- src/sound_audition_plan.h
add_executable(sound_audition_plan_test sound_audition_plan_test.cpp ../src/sound_audition_plan.cpp ../src/sound_place_plan.cpp ../src/sound_draw.cpp ../src/sound_switch.cpp ../src/sound_entries.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp)
target_include_directories(sound_audition_plan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME sound_audition_plan COMMAND sound_audition_plan_test)
# Story 13-7b -- src/reaper_view_keys.h (the Edit overlay zoom / scroll from reaper-kb.ini)
add_executable(reaper_view_keys_test reaper_view_keys_test.cpp ../src/reaper_view_keys.cpp)
target_include_directories(reaper_view_keys_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME reaper_view_keys COMMAND reaper_view_keys_test)
# Story 13-45 -- src/view_nav.h (every timeline's scroll / zoom: linked to REAPER or the user's own)
add_executable(view_nav_test view_nav_test.cpp ../src/view_nav.cpp ../src/reaper_view_keys.cpp)
target_include_directories(view_nav_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME view_nav COMMAND view_nav_test)
# Lot 13-60 -- src/action_link.h (a Shortcuts row linked to any REAPER action: its id, the list search)
add_executable(action_link_test action_link_test.cpp)
target_include_directories(action_link_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME action_link COMMAND action_link_test)
# 13-64 -- src/shortcut_key_index.h (the Shortcuts window's reverse index: Find by key, the Link search)
add_executable(shortcut_key_index_test shortcut_key_index_test.cpp ../src/shortcut_key_index.cpp ../src/view_nav.cpp
               ../src/reaper_view_keys.cpp)
target_include_directories(shortcut_key_index_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME shortcut_key_index COMMAND shortcut_key_index_test)
# Story 13-7b fix -- src/ui_drag_number.h (a DragNumber drag commits its own value)
add_executable(ui_drag_number_test ui_drag_number_test.cpp)
target_include_directories(ui_drag_number_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME ui_drag_number COMMAND ui_drag_number_test)
# 13-7 -- every symbol in a src/ string literal has a glyph in the UI font (src/ui_glyph_ranges.h)
add_executable(ui_glyphs_test ui_glyphs_test.cpp)
target_include_directories(ui_glyphs_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(ui_glyphs_test PRIVATE RAV_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../src")
add_test(NAME ui_glyphs COMMAND ui_glyphs_test)

# 13-7a / 13-7b / 13-7c -- the Library view's adaptive layout arithmetic (src/ui_flow_math.h).
add_executable(ui_flow_test ui_flow_test.cpp ../src/ui_flow_math.cpp)
target_include_directories(ui_flow_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME ui_flow COMMAND ui_flow_test)
# Story 13-15b -- src/assign_view_model.h (the sound assignment window's targets, summaries, selection)
add_executable(assign_view_model_test assign_view_model_test.cpp ../src/assign_view_model.cpp ../src/sound_assign.cpp
               ../src/sound_auto_assign.cpp ../src/sound_populate_plan.cpp ../src/sound_switch.cpp ../src/sound_draw.cpp
               ../src/sound_place_plan.cpp ../src/sound_entries.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp
               ${RAV_AUTO_SRCS} ../src/motion_physics.cpp ../src/bone_events.cpp ../src/event_list.cpp
               ../src/tagging_signal.cpp)
target_include_directories(assign_view_model_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME assign_view_model COMMAND assign_view_model_test)

# Story 13-19 -- a fast Populate with a progress bar: the gesture's live-item set, the progress
# fraction / text and the frame pacing (src/populate_progress.h).
add_executable(populate_progress_test populate_progress_test.cpp)
target_include_directories(populate_progress_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME populate_progress COMMAND populate_progress_test)

# 13-59 -- the shared busy card: its fill, text, show threshold / repaint and the memory of a long
# gesture (src/busy_progress.h).
add_executable(busy_progress_test busy_progress_test.cpp)
target_include_directories(busy_progress_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME busy_progress COMMAND busy_progress_test)

# Story 13-26 -- "Clear item data": what goes, what stays, the counts (src/item_clear_plan.h).
add_executable(item_clear_plan_test item_clear_plan_test.cpp ../src/item_clear_plan.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/sound_entries.cpp ../src/rule_record.cpp)
target_include_directories(item_clear_plan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME item_clear_plan COMMAND item_clear_plan_test)

# Story 6-5-10 -- the animation clip list's plan (src/clip_list_plan.h).
add_executable(clip_list_plan_test clip_list_plan_test.cpp ../src/clip_list_plan.cpp)
target_include_directories(clip_list_plan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME clip_list_plan COMMAND clip_list_plan_test)

# Story 11-7 -- which item REAPER plays at p, at which clip time (src/play_item_plan.h).
add_executable(play_item_plan_test play_item_plan_test.cpp ../src/play_item_plan.cpp ../src/tagging_signal.cpp)
target_include_directories(play_item_plan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME play_item_plan COMMAND play_item_plan_test)

# 13-48 -- src/library_tree_nav.h, src/library_view_keys.h
add_executable(library_tree_nav_test library_tree_nav_test.cpp ../src/library_tree_nav.cpp ../src/library_tree_edit.cpp ../src/sound_switch.cpp ../src/sound_entries.cpp ../src/sound_record.cpp ../src/sound_zones.cpp ../src/rule_record.cpp)
target_include_directories(library_tree_nav_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME library_tree_nav COMMAND library_tree_nav_test)

# 13-51 -- the shared GUID compare and growing text reader (src/reaper_item_read.h), the peak
# cache's change check (src/file_change_check.h) and the parse cache's eviction generations
# (src/evict_generation.h).
add_executable(reaper_item_read_test reaper_item_read_test.cpp ../src/reaper_item_read.cpp)
target_include_directories(reaper_item_read_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME reaper_item_read COMMAND reaper_item_read_test)
add_executable(read_robustness_test read_robustness_test.cpp)
target_include_directories(read_robustness_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME read_robustness COMMAND read_robustness_test)

# 13-55 -- the one peak reader's pure rules (src/sound_peaks_plan.h): the rate a file is read at,
# its cache key, which waiting file is read next, REAPER's peak points folded into PeakData.
add_executable(sound_peaks_plan_test sound_peaks_plan_test.cpp)
target_include_directories(sound_peaks_plan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME sound_peaks_plan COMMAND sound_peaks_plan_test)

# 13-57 -- a waveform sharp at every zoom (src/wave_tiles_plan.h): the detail tiles read from the
# samples, folded to one column per pixel, joined, the sample line, the tile LRU, the read given up.
add_executable(wave_tiles_plan_test wave_tiles_plan_test.cpp)
target_include_directories(wave_tiles_plan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME wave_tiles_plan COMMAND wave_tiles_plan_test)

# 13-62h -- src/sound_zones.h (a divided Sound's zones: the model, its gestures, the migration, the seeded fuzz).
add_executable(sound_zones_test sound_zones_test.cpp ../src/sound_zones.cpp ../src/sound_entries.cpp ../src/sound_record.cpp ../src/rule_record.cpp ../src/normalize_presets.cpp)
target_include_directories(sound_zones_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME sound_zones COMMAND sound_zones_test)
# 13-62h (Lot E1) -- src/zone_hit.h (the zone waveform's hit testing and gesture decisions).
add_executable(zone_hit_test zone_hit_test.cpp ../src/zone_hit.cpp ../src/sound_zones.cpp ../src/sound_entries.cpp ../src/sound_record.cpp ../src/rule_record.cpp ../src/normalize_presets.cpp)
target_include_directories(zone_hit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME zone_hit COMMAND zone_hit_test)

# Freeze fix #1 -- the follow's detection kept for its inputs, spread under a budget (src/detect_cache.h).
add_executable(detect_cache_test detect_cache_test.cpp)
target_include_directories(detect_cache_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME detect_cache COMMAND detect_cache_test)
# Freeze fix #1 -- the parse cache's recent files, bounded by count and size (src/asset_lru.h).
add_executable(asset_lru_test asset_lru_test.cpp)
target_include_directories(asset_lru_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME asset_lru COMMAND asset_lru_test)
# Freeze fix #2 -- an animation source's length read once per file version (src/duration_cache.h).
add_executable(duration_cache_test duration_cache_test.cpp)
target_include_directories(duration_cache_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(duration_cache_test PRIVATE Threads::Threads)
add_test(NAME duration_cache COMMAND duration_cache_test)
